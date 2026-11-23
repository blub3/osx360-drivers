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
    if re.match(r"static const UInt32 k(?:ClearSide|ClearPixels|MipSide|MipTailX|MipSampleOrigin|BaseSample\w+|SampleBackground|Trilinear\w*)\b", line))
setup = controller[controller.index("    if (trilinear) {\n      for (UInt32 index"):controller.index("    } else {\n      const UInt32 mipSampleEnd")]
source = """#include "XenosQuadProtocol.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t UInt32;
typedef uint8_t UInt8;
#define PAGE_SIZE 4096
#define CACHELINE_SIZE 128
static void IOLog(const char *, ...) {}
static void flushMarkerCacheLine(volatile UInt32 *) {}
""" + constants + "\n" + "\n".join(function_from(controller, name) for name in
    ("coordinateWord", "packQuadGeometry", "packQuadSampleQuad", "quadMaxChannelDelta", "quadHashColor",
     "quadBilinearColor", "resolveTileWord", "trilinearBilinearSample", "mipMidpoint")) + """
struct XenosController {
    UInt32 _quadGeometryMode;
    UInt32 _quadMipHash, _quadSampleHash, _quadMipMismatches, _quadMipLevel1Evidence;
    UInt32 _quadSampleMismatches, _quadMipBaseMatches, _quadMipLevel1Matches, _quadSampleGuard;
    UInt32 _quadMipChecked, _quadSampleChecked;
    UInt32 _quadTrilinearMatches, _quadTrilinearWitnesses, _quadTrilinearMaxDelta;
    void verifyQuadMipmapPass(const volatile UInt32 *, volatile UInt32 *, volatile UInt32 *);
    void verifyQuadTrilinearSamples(const volatile UInt32 *, const volatile UInt32 *, const volatile UInt32 *);
};
""" + "\n".join(function_from(controller, name) for name in ("verifyQuadMipmapPass", "verifyQuadTrilinearSamples")) + """
static float floatWord(UInt32 word) {
    float value;
    memcpy(&value, &word, sizeof value);
    return value;
}

static float sampleChannel(const UInt32 *image, float u, float v, int tailX, int bits) {
    const int x = (int)floorf(u), y = (int)floorf(v);
    const float a = u - x, b = v - y;
    float channel = 0;
    for (int dy = 0; dy <= 1; ++dy)
        for (int dx = 0; dx <= 1; ++dx)
            channel += float((image[resolveTileWord(tailX + x + dx, y + dy)] >> bits) & 0xff) *
                (dx ? a : 1 - a) * (dy ? b : 1 - b);
    return channel;
}

int main() {
    alignas(UInt32) UInt8 geometry[4 * 2 * CACHELINE_SIZE] = {};
    UInt8 *sampleGeometrySlot = geometry;
    const UInt32 kQuadGeometryStride = 2 * CACHELINE_SIZE;
    const bool trilinear = true;
""" + setup + """
    }
    UInt32 base[1024], mip[1024], sample[1024], forcedLevel0[1024];
    for (unsigned y = 0; y < 32; ++y) {
        for (unsigned x = 0; x < 32; ++x) {
            const UInt32 tile = resolveTileWord(x, y);
            base[tile] = 0xff000000U | ((53 * x + 9 * y) % 256 << 16) |
                         ((3 * x + 121 * y) % 256 << 8) | (91 * x + 33 * y) % 256;
            mip[tile] = sample[tile] = forcedLevel0[tile] = kSampleBackground;
        }
    }
    for (unsigned y = 0; y < 16; ++y)
        for (unsigned x = 0; x < 16; ++x)
            mip[resolveTileWord(x + 16, y)] = base[resolveTileWord(2 * x, 2 * y)];

    for (unsigned region = 0; region < 3; ++region) {
        const UInt8 *slot = geometry + (region + 1) * kQuadGeometryStride;
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
        assert(du * 32 == 1 && dv * 32 == 1);
        assert(kTrilinearLodSteps[region] == 16 * region);
        const float mipWeight = (float)kTrilinearLodSteps[region] / 32;
        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                const float u = u0 + (x - x0 + 0.5f) * du;
                const float v = v0 + (y - y0 + 0.5f) * dv;
                const unsigned dest = resolveTileWord(x, y);
                UInt32 color = 0, level0 = 0;
                for (int bits = 0; bits < 32; bits += 8) {
                    const float a = sampleChannel(base, u * 32 - 0.515625f, v * 32 - 0.515625f, 0, bits);
                    const float b = sampleChannel(mip, u * 16 - 0.5f, v * 16 - 0.5f, 16, bits);
                    color |= ((UInt32)lroundf(a * (1 - mipWeight) + b * mipWeight)) << bits;
                    level0 |= (UInt32)lroundf(a) << bits;
                }
                sample[dest] = color;
                forcedLevel0[dest] = region == 1 ? level0 : color;
            }
        }
    }

    // Opaque texels from the four seed-67 taps at LOD 1, region pixel (6,8).
    UInt32 seed67Mip[1024] = {};
    const UInt32 seed67Taps[] = {0xff1e0ef3, 0xff36f7b2, 0xff65f18e, 0xfff4ef2f};
    for (unsigned index = 0; index < 4; ++index)
        seed67Mip[resolveTileWord(kMipTailX + 5 + (index & 1), 7 + (index >> 1))] =
            seed67Taps[index];
    const UInt32 observedLevel1 = 0xff3577cd;
    const UInt32 oldReference = quadBilinearColor(seed67Taps, 23, 7);
    assert(oldReference == 0xff3371cf);
    assert(quadMaxChannelDelta(observedLevel1, oldReference) == 6);
    const UInt32 newReference = trilinearBilinearSample(seed67Mip, 11 * 64 + 48, 14 * 64 + 80, 1, kMipTailX);
    assert(newReference == observedLevel1);
    assert(kTrilinearTolerance == 5);
    puts("Seed 67 regression: old FF3371CF, corrected FF3577CD, observed FF3577CD.");

    XenosController clean = {};
    clean._quadGeometryMode = kXenosQuadUploadTrilinearBatch;
    clean.verifyQuadMipmapPass(base, mip, sample);
    assert(clean._quadMipChecked == 1024 && clean._quadSampleChecked == 1024);
    assert(clean._quadSampleMismatches == 0 && clean._quadMipMismatches == 0);
    assert(clean._quadMipBaseMatches == 128 && clean._quadTrilinearMatches == 128);
    assert(clean._quadMipLevel1Matches == 128 && clean._quadSampleGuard == 640);
    assert(clean._quadTrilinearWitnesses > 0 && clean._quadTrilinearMaxDelta <= 1);

    XenosController noBlend = {};
    noBlend._quadGeometryMode = kXenosQuadUploadTrilinearBatch;
    noBlend.verifyQuadMipmapPass(base, mip, forcedLevel0);
    assert(noBlend._quadMipBaseMatches == 128 && noBlend._quadMipLevel1Matches == 128);
    assert(noBlend._quadTrilinearWitnesses == 0 && noBlend._quadSampleMismatches > 64);

    XenosController corrupted = {};
    corrupted._quadGeometryMode = kXenosQuadUploadTrilinearBatch;
    sample[resolveTileWord(31, 31)] ^= 1;
    mip[resolveTileWord(0, 0)] ^= 1;
    corrupted.verifyQuadMipmapPass(base, mip, sample);
    assert(corrupted._quadMipMismatches == 1 && corrupted._quadSampleMismatches == 1);
    printf("Filtered regions and guard passed. %u of 128 pixels distinguish trilinear from either endpoint.\\n",
            clean._quadTrilinearWitnesses);
}
"""
check = root / "check-trilinear-retry.cpp"
check.write_text(source)
subprocess.run(["c++", "-std=c++11", "-Wall", "-Wextra", "-Werror", "-I", str(directory), str(check),
                "-o", str(root / "check-trilinear-retry")], check=True)
subprocess.run([str(root / "check-trilinear-retry")], check=True)
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
print(f"Conservative trilinear packet count: {count} of {budget} ring dwords.")
