from pathlib import Path
import re
import subprocess

root = Path(__file__).resolve().parent
directory = root / "source/XenonCoreGraphics/src"
controller = (directory / "XenosController.cpp").read_text()

def function_from(source, name):
    match = re.search(r"^(?:static )?\w+ (?:XenosController::)?" + name + r"\([^{}]*\)\s*(?:const\s*)?\{", source, re.M)
    assert match, name
    depth = 1
    end = match.end()
    while depth:
        char = source[end]
        end += 1
        depth += (char == "{") - (char == "}")
    return source[match.start():end]

constants = "\n".join(line for line in controller.splitlines()
    if re.match(r"static const UInt32 k(?:ClearSide|ClearPixels|MipSide|MipTailX|MipSampleOrigin|BaseSample\w+|SampleBackground) =", line))
geometry = controller[controller.index("    const XenosQuadGeometry buildMip"):
                      controller.index("    prepareQuadReadback(memory + kQuadMipOffset)")]
source = """#include "XenosQuadProtocol.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t UInt32;
typedef uint8_t UInt8;
#define PAGE_SIZE 4096
#define CACHELINE_SIZE 128
#define IOLog(...) ((void)0)
static void flushMarkerCacheLine(volatile UInt32 *) {}
""" + constants + "\n" + "\n".join(function_from(controller, name) for name in
    ("coordinateWord", "packQuadGeometry", "packQuadSampleQuad", "quadHashColor", "resolveTileWord")) + """
struct XenosController {
    UInt32 _quadMipHash, _quadSampleHash, _quadMipMismatches, _quadMipLevel1Evidence;
    UInt32 _quadSampleMismatches, _quadMipBaseMatches, _quadMipLevel1Matches, _quadSampleGuard;
    UInt32 _quadMipChecked, _quadSampleChecked;
    void verifyQuadMipmapPass(const volatile UInt32 *, volatile UInt32 *, volatile UInt32 *);
};
""" + function_from(controller, "verifyQuadMipmapPass") + """
static float floatWord(UInt32 word) {
    float value;
    memcpy(&value, &word, sizeof value);
    return value;
}

int main() {
    alignas(UInt32) UInt8 geometry[3 * 2 * CACHELINE_SIZE] = {};
    UInt8 *sampleGeometrySlot = geometry;
    const UInt32 kQuadGeometryStride = 2 * CACHELINE_SIZE;
""" + geometry + """
    UInt32 base[1024], mip[1024], sample[1024], forcedBase[1024];
    for (unsigned i = 0; i < 1024; ++i) {
        base[i] = 0xff030000 | i;
        mip[i] = sample[i] = forcedBase[i] = kSampleBackground;
    }
    for (unsigned quad = 0; quad < 3; ++quad) {
        const UInt8 *slot = geometry + quad * kQuadGeometryStride;
        const UInt32 *position = reinterpret_cast<const UInt32 *>(slot);
        const UInt32 *uv = reinterpret_cast<const UInt32 *>(slot + CACHELINE_SIZE);
        const int x0 = (floatWord(position[6]) + 1) * 16;
        const int x1 = (floatWord(position[3]) + 1) * 16;
        const int y0 = (1 - floatWord(position[7])) * 16;
        const int y1 = (1 - floatWord(position[4])) * 16;
        const float u0 = floatWord(uv[4]);
        const float v0 = floatWord(uv[5]);
        const float du = (floatWord(uv[2]) - u0) / (x1 - x0);
        const float dv = (floatWord(uv[3]) - v0) / (y1 - y0);
        assert(fabsf(du) * 32 == (quad < 2 ? 2 : 1));
        assert(fabsf(dv) * 32 == (quad < 2 ? 2 : 1));
        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                const float u = u0 + (x - x0 + 0.5f) * du;
                const float v = v0 + (y - y0 + 0.5f) * dv;
                const unsigned destination = resolveTileWord(x, y);
                const UInt32 baseColor = base[resolveTileWord(floorf(u * 32), floorf(v * 32))];
                if (quad == 0) {
                    mip[destination] = baseColor;
                } else {
                    sample[destination] = quad == 1
                        ? mip[resolveTileWord(16 + floorf(u * 16), floorf(v * 16))] : baseColor;
                    forcedBase[destination] = baseColor;
                }
            }
        }
    }
    XenosController verified = {};
    verified.verifyQuadMipmapPass(base, mip, sample);
    assert(verified._quadMipChecked == 1024 && verified._quadSampleChecked == 1024);
    assert(verified._quadMipMismatches == 0 && verified._quadSampleMismatches == 0);
    assert(verified._quadMipBaseMatches == 144 && verified._quadMipLevel1Matches == 256);
    assert(verified._quadSampleGuard == 624 && verified._quadMipLevel1Evidence == 256);

    XenosController ignoredMip = {};
    ignoredMip.verifyQuadMipmapPass(base, mip, forcedBase);
    assert(ignoredMip._quadMipMismatches == 0 && ignoredMip._quadSampleMismatches == 256);
    assert(ignoredMip._quadMipLevel1Evidence == 0 && ignoredMip._quadMipBaseMatches == 144);

    mip[resolveTileWord(0, 0)] ^= 1;
    sample[resolveTileWord(31, 31)] ^= 1;
    sample[resolveTileWord(19, 19)] ^= 1;
    XenosController corrupt = {};
    corrupt.verifyQuadMipmapPass(base, mip, sample);
    assert(corrupt._quadMipMismatches == 1 && corrupt._quadSampleMismatches == 2);
    assert(corrupt._quadMipBaseMatches == 143 && corrupt._quadSampleGuard == 623);
    puts("Verifier checks packed mip, L0, L1, guard pixels, and rejects forced L0 and corruption.");
}
"""
check = root / "check-mipmap.cpp"
check.write_text(source)
subprocess.run(["c++", "-std=c++11", "-Wall", "-Wextra", "-Werror", "-I", str(directory), str(check),
                "-o", str(root / "check-mipmap")], check=True)
subprocess.run([str(root / "check-mipmap")], check=True)

sources = "\n".join(path.read_text() for path in directory.glob("*.cpp"))
fixed_costs = {"writeRing": 1, "writeRingPacket0": 1, "writeRingPacket3": 1, "writeRingRegister": 2}
functions = set(re.findall(r"^\w+ XenosController::(\w+)\(", sources, re.M))

def ring_cost(name):
    if name in fixed_costs:
        return fixed_costs[name]
    body = function_from(sources, name).split("{", 1)[1]
    calls = re.findall(r"\b(\w+)\(", body)
    return sum(ring_cost(call) for call in calls if call in fixed_costs or call in functions)

count = (ring_cost("submitQuad") - ring_cost("drawQuadTexturePass") +
         ring_cost("setQuadBlendState") + 3 * ring_cost("drawTextureLayer"))
budget = int(re.search(r"kQuadMipmapRingDwords = (\d+)", controller)[1])
assert count <= budget
print(f"Conservative mipmap packet count: {count} of {budget} ring dwords.")
