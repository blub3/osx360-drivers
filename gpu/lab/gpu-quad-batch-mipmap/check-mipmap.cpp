#include "XenosQuadProtocol.h"
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
static const UInt32 kClearSide = kXenosQuadTargetSide;
static const UInt32 kClearPixels = kXenosQuadReadbackPixels;
static const UInt32 kMipSide = kClearSide / 2;
static const UInt32 kMipTailX = 16;
static const UInt32 kMipSampleOrigin = 1;
static const UInt32 kBaseSampleOrigin = 19;
static const UInt32 kBaseSampleSide = 12;
static const UInt32 kBaseSampleU = 3;
static const UInt32 kBaseSampleV = 4;
static const UInt32 kSampleBackground = 0xff213447;
static UInt32 coordinateWord(int value, unsigned fractionalBits) {
  if (!value)
    return 0;
  const UInt32 magnitude = value < 0 ? -value : value;
  const unsigned highBit = 31 - __builtin_clz(static_cast<unsigned>(magnitude));
  const UInt32 exponent = (127 + highBit - fractionalBits) << 23;
  const UInt32 mantissa = (magnitude << (23 - highBit)) & 0x007fffffU;
  return (value < 0 ? 0x80000000U : 0) | exponent | mantissa;
}
static void packQuadGeometry(const XenosQuadGeometry &geometry, UInt32 *positions,
                             UInt32 *uvs, bool rotated) {
  const int half = rotated ? 64 : 16;
  const unsigned fractionalBits = rotated ? 6 : 4;
  for (UInt32 index = 0; index < 4; ++index) {
    const XenosQuadVertex &vertex = geometry.vertices[index];
    // Positions use clip space. The sampler uses a padded 32x32 texture.
    positions[3 * index] = coordinateWord(int(vertex.x) - half, fractionalBits);
    positions[3 * index + 1] = coordinateWord(half - int(vertex.y), fractionalBits);
    positions[3 * index + 2] = 0;
    uvs[2 * index] = coordinateWord(vertex.u, 5);
    uvs[2 * index + 1] = coordinateWord(vertex.v, 5);
  }
}
static void packQuadSampleQuad(UInt8 *slot, const XenosQuadGeometry &geometry,
                                  int uOffsetQuarter, int vOffsetQuarter) {
  UInt32 *uv = reinterpret_cast<UInt32 *>(slot + CACHELINE_SIZE);
  packQuadGeometry(geometry, reinterpret_cast<UInt32 *>(slot), uv, false);
  // Encode quarter-texel offsets, normalized by the 32x32 base size.
  for (UInt32 index = 0; index < 4; ++index) {
    const XenosQuadVertex &vertex = geometry.vertices[index];
    uv[2 * index] = coordinateWord(4 * int(vertex.u) + uOffsetQuarter, 7);
    uv[2 * index + 1] = coordinateWord(4 * int(vertex.v) + vOffsetQuarter, 7);
  }
}
static UInt32 quadHashColor(UInt32 hash, UInt32 color) {
  hash = xenosQuadHashByte(hash, (color >> 16) & 0xff);
  hash = xenosQuadHashByte(hash, (color >> 8) & 0xff);
  return xenosQuadHashByte(hash, color & 0xff);
}
static UInt32 resolveTileWord(UInt32 x, UInt32 y) {
  const UInt32 pipe = (x >> 3) ^ ((y & 8) >> 2);
  const UInt32 rows = (y & 30) << 5;
  const UInt32 withinPipe = ((x & 4) << 1) | ((y & 1) << 2) | (x & 3);
  return rows | (pipe << 4) | withinPipe;
}
struct XenosController {
    UInt32 _quadMipHash, _quadSampleHash, _quadMipMismatches, _quadMipLevel1Evidence;
    UInt32 _quadSampleMismatches, _quadMipBaseMatches, _quadMipLevel1Matches, _quadSampleGuard;
    UInt32 _quadMipChecked, _quadSampleChecked;
    void verifyQuadMipmapPass(const volatile UInt32 *, volatile UInt32 *, volatile UInt32 *);
};
void XenosController::verifyQuadMipmapPass(const volatile UInt32 *base, volatile UInt32 *mip,
                                             volatile UInt32 *sample) {
  for (UInt32 offset = 0; offset < PAGE_SIZE; offset += CACHELINE_SIZE) {
    flushMarkerCacheLine(mip + offset / sizeof(UInt32));
    flushMarkerCacheLine(sample + offset / sizeof(UInt32));
  }

  _quadMipHash = _quadSampleHash = XENOS_QUAD_HASH_INITIAL;
  for (UInt32 y = 0; y < kClearSide; ++y) {
    for (UInt32 x = 0; x < kClearSide; ++x) {
      const UInt32 slot = resolveTileWord(x, y);
      const bool inTail = x >= kMipTailX && y < kMipSide;
      const UInt32 expectedMip = inTail ? base[resolveTileWord(2 * (x - kMipTailX), 2 * y)]
                                         : kSampleBackground;
      const UInt32 mipPixel = mip[slot];
      if (mipPixel != expectedMip) {
        if (!_quadMipMismatches)
          IOLog("XenosBatchMipmap: tail mismatch=(%lu,%lu) expected=%08lx observed=%08lx\n",
                x, y, expectedMip, mipPixel);
        ++_quadMipMismatches;
      }
      _quadMipHash = quadHashColor(_quadMipHash, mipPixel);

      const bool level1 = x >= kMipSampleOrigin && x < kMipSampleOrigin + kMipSide &&
                          y >= kMipSampleOrigin && y < kMipSampleOrigin + kMipSide;
      const bool level0 = x >= kBaseSampleOrigin && x < kBaseSampleOrigin + kBaseSampleSide &&
                          y >= kBaseSampleOrigin && y < kBaseSampleOrigin + kBaseSampleSide;
      const UInt32 observed = sample[slot];
      UInt32 expected = kSampleBackground;
      if (level1) {
        const UInt32 u = kMipSide - 1 - (x - kMipSampleOrigin), v = y - kMipSampleOrigin;
        expected = mip[resolveTileWord(kMipTailX + u, v)];
        // At these UVs a direct base lookup lands on the odd texel, while
        // level 1 was prepared from the even texel in each 2x2 footprint.
        _quadMipLevel1Evidence += observed == expected &&
                                  observed != base[resolveTileWord(2 * u + 1, 2 * v + 1)];
      } else if (level0) {
        expected = base[resolveTileWord(x - kBaseSampleOrigin + kBaseSampleU,
                                         y - kBaseSampleOrigin + kBaseSampleV)];
      }

      const bool matched = observed == expected;
      if (!matched) {
        if (!_quadSampleMismatches)
          IOLog("XenosBatchMipmap: sample mismatch=(%lu,%lu) expected=%08lx observed=%08lx\n",
                x, y, expected, observed);
        ++_quadSampleMismatches;
      }
      _quadMipBaseMatches += matched && level0;
      _quadMipLevel1Matches += matched && level1;
      _quadSampleGuard += matched && !level0 && !level1;
      _quadSampleHash = quadHashColor(_quadSampleHash, observed);
    }
  }
  _quadMipChecked = _quadSampleChecked = kClearPixels;
}
static float floatWord(UInt32 word) {
    float value;
    memcpy(&value, &word, sizeof value);
    return value;
}

int main() {
    alignas(UInt32) UInt8 geometry[3 * 2 * CACHELINE_SIZE] = {};
    UInt8 *sampleGeometrySlot = geometry;
    const UInt32 kQuadGeometryStride = 2 * CACHELINE_SIZE;
    const XenosQuadGeometry buildMip =
        xenosQuadMakeGeometry(kMipTailX, 0, kMipTailX + kMipSide, kMipSide, 0, 0, kClearSide, kClearSide);
    const UInt32 mipSampleEnd = kMipSampleOrigin + kMipSide;
    const XenosQuadGeometry sampleMip =
        xenosQuadMakeGeometry(kMipSampleOrigin, kMipSampleOrigin, mipSampleEnd, mipSampleEnd,
                              kClearSide, 0, 0, kClearSide);
    const UInt32 baseSampleEnd = kBaseSampleOrigin + kBaseSampleSide;
    const XenosQuadGeometry sampleBase =
        xenosQuadMakeGeometry(kBaseSampleOrigin, kBaseSampleOrigin, baseSampleEnd, baseSampleEnd,
                              kBaseSampleU, kBaseSampleV, kBaseSampleU + kBaseSampleSide,
                              kBaseSampleV + kBaseSampleSide);
    packQuadSampleQuad(sampleGeometrySlot, buildMip, -1, -1);
    packQuadSampleQuad(sampleGeometrySlot + kQuadGeometryStride, sampleMip, 1, 1);
    packQuadSampleQuad(sampleGeometrySlot + 2 * kQuadGeometryStride, sampleBase, 0, 0);

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
