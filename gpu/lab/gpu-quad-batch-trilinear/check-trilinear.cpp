#include "XenosQuadProtocol.h"
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
static const UInt32 kClearSide = kXenosQuadTargetSide;
static const UInt32 kClearPixels = kXenosQuadReadbackPixels;
static const UInt32 kMipSide = kClearSide / 2;
static const UInt32 kMipTailX = 16;
static const UInt32 kTrilinearRegionCount = 3;
static const UInt32 kTrilinearRegionX[] = {1, 11, 21};
static const UInt32 kTrilinearRegionY = 1;
static const UInt32 kTrilinearWidth = 8;
static const UInt32 kTrilinearHeight = 16;
static const UInt32 kTrilinearU = 5;
static const UInt32 kTrilinearV = 6;
static const UInt32 kTrilinearUQuarter = 1;
static const UInt32 kTrilinearVQuarter = 3;
static const UInt32 kTrilinearLodSteps[] = {0, 16, 32};
static const UInt32 kTrilinearTolerance = 5;
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
static UInt32 quadMaxChannelDelta(UInt32 observed, UInt32 expected) {
  UInt32 maximum = 0;
  for (unsigned shift = 0; shift < 32; shift += 8) {
    const UInt32 a = (observed >> shift) & 0xff;
    const UInt32 b = (expected >> shift) & 0xff;
    const UInt32 delta = a > b ? a - b : b - a;
    if (delta > maximum)
      maximum = delta;
  }
  return maximum;
}
static UInt32 quadHashColor(UInt32 hash, UInt32 color) {
  hash = xenosQuadHashByte(hash, (color >> 16) & 0xff);
  hash = xenosQuadHashByte(hash, (color >> 8) & 0xff);
  return xenosQuadHashByte(hash, color & 0xff);
}
static UInt32 quadBilinearColor(const UInt32 *samples, UInt32 u, UInt32 v) {
  const UInt32 scale = 64;
  const UInt32 weights[] = {(scale - u) * (scale - v), u * (scale - v), (scale - u) * v, u * v};
  const UInt32 totalWeight = scale * scale;
  UInt32 color = 0;
  for (UInt32 shift = 0; shift < 32; shift += 8) {
    UInt32 sum = 0;
    for (UInt32 index = 0; index < 4; ++index)
      sum += ((samples[index] >> shift) & 0xff) * weights[index];
    color |= ((sum + totalWeight / 2) / totalWeight) << shift;
  }
  return color;
}
static UInt32 resolveTileWord(UInt32 x, UInt32 y) {
  const UInt32 pipe = (x >> 3) ^ ((y & 8) >> 2);
  const UInt32 rows = (y & 30) << 5;
  const UInt32 withinPipe = ((x & 4) << 1) | ((y & 1) << 2) | (x & 3);
  return rows | (pipe << 4) | withinPipe;
}
static UInt32 trilinearBilinearSample(const volatile UInt32 *image, UInt32 u, UInt32 v,
                                     UInt32 level, UInt32 tailX) {
  // Convert normalized coordinates to 6-bit texel fractions and use the
  // measured sampler phase. The diagnostic UVs stay inside both mip levels.
  u = (u >> level) - 33;
  v = (v >> level) - 33;
  const UInt32 x = tailX + (u >> 6), y = v >> 6;
  const UInt32 taps[] = {image[resolveTileWord(x, y)], image[resolveTileWord(x + 1, y)],
                         image[resolveTileWord(x, y + 1)], image[resolveTileWord(x + 1, y + 1)]};
  return quadBilinearColor(taps, u & 63, v & 63);
}
static UInt32 mipMidpoint(UInt32 level0, UInt32 level1) {
  UInt32 result = 0;
  for (UInt32 shift = 0; shift < 32; shift += 8) {
    const UInt32 sum = ((level0 >> shift) & 0xff) + ((level1 >> shift) & 0xff);
    result |= ((sum + 1) / 2) << shift;
  }
  return result;
}
struct XenosController {
    UInt32 _quadGeometryMode;
    UInt32 _quadMipHash, _quadSampleHash, _quadMipMismatches, _quadMipLevel1Evidence;
    UInt32 _quadSampleMismatches, _quadMipBaseMatches, _quadMipLevel1Matches, _quadSampleGuard;
    UInt32 _quadMipChecked, _quadSampleChecked;
    UInt32 _quadTrilinearMatches, _quadTrilinearWitnesses, _quadTrilinearMaxDelta;
    void verifyQuadMipmapPass(const volatile UInt32 *, volatile UInt32 *, volatile UInt32 *);
    void verifyQuadTrilinearSamples(const volatile UInt32 *, const volatile UInt32 *, const volatile UInt32 *);
};
void XenosController::verifyQuadMipmapPass(const volatile UInt32 *base, volatile UInt32 *mip,
                                             volatile UInt32 *sample) {
  for (UInt32 offset = 0; offset < PAGE_SIZE; offset += CACHELINE_SIZE) {
    flushMarkerCacheLine(mip + offset / sizeof(UInt32));
    flushMarkerCacheLine(sample + offset / sizeof(UInt32));
  }

  const bool trilinear = _quadGeometryMode == kXenosQuadUploadTrilinearBatch;
  const char *log = trilinear ? "XenosBatchTrilinear" : "XenosBatchMipmap";
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
          IOLog("%s: tail mismatch=(%lu,%lu) expected=%08lx observed=%08lx\n",
                log, x, y, expectedMip, mipPixel);
        ++_quadMipMismatches;
      }
      _quadMipHash = quadHashColor(_quadMipHash, mipPixel);
      if (trilinear)
        continue;

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
  if (trilinear)
    verifyQuadTrilinearSamples(base, mip, sample);
  _quadMipChecked = _quadSampleChecked = kClearPixels;
}
void XenosController::verifyQuadTrilinearSamples(const volatile UInt32 *base, const volatile UInt32 *mip,
                                                  const volatile UInt32 *sample) {
  for (UInt32 y = 0; y < kTrilinearHeight; ++y) {
    for (UInt32 x = 0; x < kTrilinearWidth; ++x) {
      const UInt32 u = kTrilinearU + x;
      const UInt32 v = kTrilinearV + y;
      const UInt32 drawY = kTrilinearRegionY + y;
      const UInt32 observed0 = sample[resolveTileWord(kTrilinearRegionX[0] + x, drawY)];
      const UInt32 observedMid = sample[resolveTileWord(kTrilinearRegionX[1] + x, drawY)];
      const UInt32 observed1 = sample[resolveTileWord(kTrilinearRegionX[2] + x, drawY)];
      const UInt32 offsetsU = 32 + kTrilinearUQuarter * 16;
      const UInt32 offsetsV = 32 + kTrilinearVQuarter * 16;
      const UInt32 expected0 = trilinearBilinearSample(base, u * 64 + offsetsU, v * 64 + offsetsV, 0, 0);
      const UInt32 expected1 = trilinearBilinearSample(mip, u * 64 + offsetsU, v * 64 + offsetsV, 1, kMipTailX);
      const UInt32 expectedMid = mipMidpoint(observed0, observed1);
      const UInt32 delta0 = quadMaxChannelDelta(observed0, expected0);
      const UInt32 deltaMid = quadMaxChannelDelta(observedMid, expectedMid);
      const UInt32 delta1 = quadMaxChannelDelta(observed1, expected1);
      const bool match0 = delta0 <= kTrilinearTolerance;
      const bool matchMid = deltaMid <= kTrilinearTolerance;
      const bool match1 = delta1 <= kTrilinearTolerance;
      if (!_quadSampleMismatches && (!match0 || !matchMid || !match1)) {
        IOLog("XenosBatchTrilinear: sample=(%lu,%lu) level0=%08lx/%08lx mid=%08lx/%08lx level1=%08lx/%08lx\n",
              x, y, observed0, expected0, observedMid, expectedMid, observed1, expected1);
      }
      _quadSampleMismatches += !match0 + !matchMid + !match1;
      _quadMipBaseMatches += match0;
      _quadTrilinearMatches += matchMid;
      _quadMipLevel1Matches += match1;
      _quadTrilinearWitnesses += match0 && matchMid && match1 &&
          quadMaxChannelDelta(observedMid, observed0) > kTrilinearTolerance &&
          quadMaxChannelDelta(observedMid, observed1) > kTrilinearTolerance;
      if (deltaMid > _quadTrilinearMaxDelta)
        _quadTrilinearMaxDelta = deltaMid;
    }
  }
  for (UInt32 y = 0; y < kClearSide; ++y) {
    for (UInt32 x = 0; x < kClearSide; ++x) {
      const UInt32 observed = sample[resolveTileWord(x, y)];
      _quadSampleHash = quadHashColor(_quadSampleHash, observed);
      bool inside = false;
      for (UInt32 index = 0; index < kTrilinearRegionCount; ++index)
        inside |= y >= kTrilinearRegionY && y < kTrilinearRegionY + kTrilinearHeight &&
                  x >= kTrilinearRegionX[index] && x < kTrilinearRegionX[index] + kTrilinearWidth;
      if (!inside) {
        _quadSampleMismatches += observed != kSampleBackground;
        _quadSampleGuard += observed == kSampleBackground;
      }
    }
  }
}
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
    if (trilinear) {
      for (UInt32 index = 0; index < kTrilinearRegionCount; ++index) {
        const UInt32 left = kTrilinearRegionX[index];
        const XenosQuadGeometry region = xenosQuadMakeGeometry(
            left, kTrilinearRegionY, left + kTrilinearWidth, kTrilinearRegionY + kTrilinearHeight,
            kTrilinearU, kTrilinearV, kTrilinearU + kTrilinearWidth, kTrilinearV + kTrilinearHeight);
        packQuadSampleQuad(sampleGeometrySlot + (index + 1) * kQuadGeometryStride, region,
                           kTrilinearUQuarter, kTrilinearVQuarter);
      }

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
                    const float b = sampleChannel(mip, u * 16 - 0.515625f, v * 16 - 0.515625f, 16, bits);
                    color |= ((UInt32)lroundf(a * (1 - mipWeight) + b * mipWeight)) << bits;
                    level0 |= (UInt32)lroundf(a) << bits;
                }
                sample[dest] = color;
                forcedLevel0[dest] = region == 1 ? level0 : color;
            }
        }
    }

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
    printf("Filtered regions and guard passed. %u of 128 pixels distinguish trilinear from either endpoint.\n",
            clean._quadTrilinearWitnesses);
}
