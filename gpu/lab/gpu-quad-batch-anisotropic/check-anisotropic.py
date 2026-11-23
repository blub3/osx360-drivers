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
    if re.match(r"static const UInt32 k(?:ClearSide|ClearPixels|SampleBackground|Aniso\w+)\b", line))
start = controller.index("    for (UInt32 index = 0; index < kAnisoRegionCount; ++index)")
geometry = controller[start:controller.index("    volatile UInt32 *striped", start)]
source = """#include "XenosQuadProtocol.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t UInt32;
typedef uint8_t UInt8;
#define PAGE_SIZE 4096
#define CACHELINE_SIZE 128
static void IOLog(const char *, ...) {}
static void flushMarkerCacheLine(volatile UInt32 *) {}
""" + constants + "\n" + "\n".join(function_from(controller, name) for name in (
    "coordinateWord", "packQuadGeometry", "packQuadSampleQuad", "quadMaxChannelDelta", "quadHashColor",
    "resolveTileWord", "anisotropicStripeColor", "anisotropicQuadCoversPixel")) + """
struct XenosController {
    UInt32 _quadAnisoControl, _quadAnisoDark, _quadAnisoLight, _quadAnisoDarkMax;
    UInt32 _quadAnisoLightMin, _quadSampleGuard, _quadSampleHash;
    UInt32 _quadSampleChecked, _quadSampleMismatches;
    void verifyQuadAnisotropicPass(volatile UInt32 *);
};
""" + function_from(controller, "verifyQuadAnisotropicPass") + """
static float floatWord(UInt32 word) {
    float value;
    memcpy(&value, &word, sizeof value);
    return value;
}

int main() {
    assert(kAniso16To1 == 5);
    alignas(UInt32) UInt8 storage[2 * 2 * CACHELINE_SIZE] = {};
    UInt8 *sampleGeometrySlot = storage;
    const UInt32 kQuadGeometryStride = 2 * CACHELINE_SIZE;
""" + geometry + """
    UInt32 good[1024], disabled[1024], swapped[1024];
    for (unsigned pixel = 0; pixel < 1024; ++pixel)
        good[pixel] = disabled[pixel] = swapped[pixel] = kSampleBackground;
    const UInt32 gray = 0xff8060a0;
    for (unsigned region = 0; region < 2; ++region) {
        const UInt8 *slot = storage + region * kQuadGeometryStride;
        const UInt32 *position = reinterpret_cast<const UInt32 *>(slot);
        const UInt32 *uv = reinterpret_cast<const UInt32 *>(slot + CACHELINE_SIZE);
        const float left = 16 * (1 + floatWord(position[6]));
        const float top = 16 * (1 - floatWord(position[7]));
        const float dx = 16 * (1 + floatWord(position[9])) - left;
        const float dy = 16 * (1 - floatWord(position[10])) - top;
        const float height = 16 * (1 - floatWord(position[1])) - top;
        const float u0 = 32 * floatWord(uv[4]);
        const float v0 = 32 * floatWord(uv[5]);
        const float dU = 32 * floatWord(uv[6]) - u0;
        const float dV = 32 * floatWord(uv[1]) - v0;
        assert(dU / dx == 1.0f && dV / height == 8.0f && dy / dx == 0.25f);
        for (unsigned y = 0; y < 32; ++y) {
            for (unsigned x = 0; x < 32; ++x) {
                const float p = (x + 0.5f - left) / dx;
                const float q = (y + 0.5f - top - p * dy) / height;
                if (p < 0 || p > 1 || q < 0 || q > 1)
                    continue;
                const UInt32 pixel = resolveTileWord(x, y);
                const unsigned u = u0 + p * dU;
                good[pixel] = region ? anisotropicStripeColor(u) : gray;
                disabled[pixel] = gray;
                swapped[pixel] = region ? anisotropicStripeColor(u ^ 1) : gray;
            }
        }
    }
    XenosController pass = {};
    pass._quadAnisoLightMin = 255;
    pass.verifyQuadAnisotropicPass(good);
    assert(pass._quadSampleChecked == 1024 && pass._quadSampleMismatches == 0);
    assert(pass._quadAnisoControl == 72 && pass._quadAnisoDark == 36 && pass._quadAnisoLight == 36);
    assert(pass._quadSampleGuard == 880 && pass._quadAnisoDarkMax == 16 && pass._quadAnisoLightMin == 240);
    XenosController off = {};
    off.verifyQuadAnisotropicPass(disabled);
    assert(off._quadAnisoControl == 72 && off._quadSampleMismatches == 72);
    XenosController wrongStripes = {};
    wrongStripes.verifyQuadAnisotropicPass(swapped);
    assert(wrongStripes._quadSampleMismatches == 72);
    XenosController wrongCoverage = {};
    good[resolveTileWord(0, 0)] ^= 1;
    wrongCoverage.verifyQuadAnisotropicPass(good);
    assert(wrongCoverage._quadSampleMismatches == 1 && wrongCoverage._quadSampleGuard == 879);
    puts("Anisotropic check passes both angled quads and rejects disabled filtering, reversed stripes, and bad coverage.");
}
"""
check = root / "check-anisotropic.cpp"
check.write_text(source)
subprocess.run(["c++", "-std=c++11", "-Wall", "-Wextra", "-Werror", "-I", str(directory), str(check),
                "-o", str(root / "check-anisotropic")], check=True)
subprocess.run([str(root / "check-anisotropic")], check=True)

transmit = controller.index("  flushQuadResolve(readback);", controller.index("bool XenosController::submitQuad()"))
other_modes = controller.index("  } else if (mipmap) {", transmit)
state_restore = controller.index("\n  writeRingPacket0(kXenosCoherDestBase1, 1);", other_modes)
anisotropic = controller[:other_modes] + "  }\n" + controller[state_restore:]
sources = anisotropic + "\n" + "\n".join(path.read_text() for path in directory.glob("*.cpp")
                                           if path.name != "XenosController.cpp")
fixed_costs = {"writeRing": 1, "writeRingPacket0": 1, "writeRingPacket3": 1, "writeRingRegister": 2}
functions = set(re.findall(r"^\w+ XenosController::(\w+)\(", sources, re.M))

def ring_cost(name):
    if name in fixed_costs:
        return fixed_costs[name]
    body = function_from(sources, name).split("{", 1)[1]
    calls = re.findall(r"\b(\w+)\(", body)
    return sum(ring_cost(call) for call in calls if call in fixed_costs or call in functions)

count = ring_cost("submitQuad") + ring_cost("setQuadBlendState") + 3 * ring_cost("drawTextureLayer")
budget = int(re.search(r"kQuadMipmapRingDwords = (\d+)", controller)[1])
assert count <= budget
print(f"Conservative anisotropic packet count: {count} of {budget} ring dwords.")
