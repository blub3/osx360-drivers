#include "XenosQuadProtocol.h"
#include <assert.h>
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
static const UInt32 kAnisoRegionCount = 2;
static const UInt32 kAnisoLeft[] = {2, 18};
static const UInt32 kAnisoTop = 5;
static const UInt32 kAnisoWidth = 12;
static const UInt32 kAnisoHeight = 6;
static const UInt32 kAnisoSlopeDenominator = 4;
static const UInt32 kAnisoU = 8;
static const UInt32 kAnisoVSpan = 8 * kAnisoHeight;
static const UInt32 kAniso16To1 = 5; // Fetch constant encoding.
static const UInt32 kAnisoColorTag = 0xff0060a0;
static const UInt32 kAnisoDarkRed = 16;
static const UInt32 kAnisoLightRed = 240;
static const UInt32 kAnisoGrayRed = (kAnisoDarkRed + kAnisoLightRed) / 2;
static const UInt32 kAnisoColorTolerance = 2;
static const UInt32 kAnisoMinStripeDelta = 32;
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
static UInt32 resolveTileWord(UInt32 x, UInt32 y) {
  const UInt32 pipe = (x >> 3) ^ ((y & 8) >> 2);
  const UInt32 rows = (y & 30) << 5;
  const UInt32 withinPipe = ((x & 4) << 1) | ((y & 1) << 2) | (x & 3);
  return rows | (pipe << 4) | withinPipe;
}
static UInt32 anisotropicStripeColor(UInt32 x) {
  return kAnisoColorTag | ((x & 1 ? kAnisoLightRed : kAnisoDarkRed) << 16);
}
static bool anisotropicQuadCoversPixel(UInt32 x, UInt32 y, UInt32 left) {
  if (x < left || x >= left + kAnisoWidth)
    return false;
  // Use doubled pixel-center coordinates to account for the 1:4 tilt.
  const int alongY = int(kAnisoSlopeDenominator) * (2 * (int(y) - int(kAnisoTop)) + 1) -
                     (2 * int(x - left) + 1);
  return alongY > 0 && alongY < int(2 * kAnisoSlopeDenominator * kAnisoHeight);
}
struct XenosController {
    UInt32 _quadAnisoControl, _quadAnisoDark, _quadAnisoLight, _quadAnisoDarkMax;
    UInt32 _quadAnisoLightMin, _quadSampleGuard, _quadSampleHash;
    UInt32 _quadSampleChecked, _quadSampleMismatches;
    void verifyQuadAnisotropicPass(volatile UInt32 *);
};
void XenosController::verifyQuadAnisotropicPass(volatile UInt32 *sample) {
  for (UInt32 offset = 0; offset < PAGE_SIZE; offset += CACHELINE_SIZE)
    flushMarkerCacheLine(sample + offset / sizeof(UInt32));

  _quadSampleHash = XENOS_QUAD_HASH_INITIAL;
  const UInt32 controlGray = kAnisoColorTag | (kAnisoGrayRed << 16);
  for (UInt32 y = 0; y < kClearSide; ++y) {
    for (UInt32 x = 0; x < kClearSide; ++x) {
      const UInt32 observed = sample[resolveTileWord(x, y)];
      const bool control = anisotropicQuadCoversPixel(x, y, kAnisoLeft[0]);
      const bool stripes = anisotropicQuadCoversPixel(x, y, kAnisoLeft[1]);
      bool matched = observed == kSampleBackground;
      if (control) {
        matched = quadMaxChannelDelta(observed, controlGray) <= kAnisoColorTolerance;
        _quadAnisoControl += matched;
      } else if (stripes) {
        const UInt32 red = (observed >> 16) & 0xff;
        const bool light = (kAnisoU + x - kAnisoLeft[1]) & 1;
        const bool tag = quadMaxChannelDelta(observed & 0xff00ffffU, kAnisoColorTag) <= kAnisoColorTolerance;
        matched = tag && (light ? red >= kAnisoGrayRed + kAnisoMinStripeDelta
                                : red <= kAnisoGrayRed - kAnisoMinStripeDelta);
        if (light) {
          _quadAnisoLight += matched;
          if (red < _quadAnisoLightMin)
            _quadAnisoLightMin = red;
        } else {
          _quadAnisoDark += matched;
          if (red > _quadAnisoDarkMax)
            _quadAnisoDarkMax = red;
        }
      } else {
        _quadSampleGuard += matched;
      }
      if (!matched && !_quadSampleMismatches)
        IOLog("XenosBatchAnisotropic: sample mismatch=(%lu,%lu) observed=%08lx control=%u stripes=%u\n",
              x, y, observed, control, stripes);
      _quadSampleMismatches += !matched;
      _quadSampleHash = quadHashColor(_quadSampleHash, observed);
    }
  }
  _quadSampleChecked = kClearPixels;
}
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
    for (UInt32 index = 0; index < kAnisoRegionCount; ++index) {
      const UInt32 left = kAnisoLeft[index];
      XenosQuadGeometry quad = xenosQuadMakeGeometry(left, kAnisoTop, left + kAnisoWidth,
          kAnisoTop + kAnisoHeight, kAnisoU, 0, kAnisoU + kAnisoWidth, kAnisoVSpan);
      quad.vertices[1].y += kAnisoWidth / kAnisoSlopeDenominator;
      quad.vertices[3].y += kAnisoWidth / kAnisoSlopeDenominator;
      packQuadSampleQuad(sampleGeometrySlot + index * kQuadGeometryStride, quad, 0, 0);
    }

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
