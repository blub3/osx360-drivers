#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include "XenosQuadProtocol.h"
typedef uint32_t UInt32;
typedef uint8_t UInt8;
struct QuadLayer {
    XenosQuadGeometryUpload payload;
    XenosQuadMapping mapping;
    XenosQuadRotation rotation;
    bool rotated;
};
static QuadLayer _quadLayers[2];
static UInt8 _quadFrontAlpha[kXenosQuadTexturePixels];
static const UInt32 _quadGeometryMode = kXenosQuadUploadBlendedBatch, kQuadSide = kXenosQuadUploadSide;
static void generateRGB(uint8_t *rgb, uint32_t seed) {
    for (unsigned i = 0; i < kXenosQuadUploadBytes; ++i) {
        seed = seed * 1664525U + 1013904223U;
        rgb[i] = (uint8_t)(seed >> 24);
    }
}

enum {
    kLowAlpha = 64,
    kHighAlpha = 192,
};

static void makeBatch(XenosQuadBlendedBatchUpload *upload, uint32_t seed) {
    XenosQuadBatchLayer *back = &upload->batch.layers[0];
    XenosQuadBatchLayer *front = &upload->batch.layers[1];
    back->kind = front->kind = kXenosQuadAxisAligned;
    back->payload.geometry = xenosQuadMakeGeometry(2, 3, 18, 19, 0, 0, 16, 16);
    front->payload.geometry = xenosQuadMakeGeometry(11, 9, 29, 29, 12, 2, 3, 12);
    generateRGB(back->payload.rgb, seed);
    generateRGB(front->payload.rgb, seed ^ 0x9e3779b9U);
    for (unsigned v = 0; v < kXenosQuadUploadSide; ++v)
        for (unsigned u = 0; u < kXenosQuadUploadSide; ++u)
            upload->frontAlpha[v * kXenosQuadUploadSide + u] =
                ((u / 4) ^ (v / 4)) & 1 ? kHighAlpha : kLowAlpha;
}

// Evaluate the UV at the center of the pixel using the submitted vertices.
static int sampleIndex(const XenosQuadGeometry *geometry, int x, int y) {
    const XenosQuadVertex topLeft = geometry->vertices[2];
    const XenosQuadVertex bottomRight = geometry->vertices[1];
    if (x < topLeft.x || x >= bottomRight.x || y < topLeft.y || y >= bottomRight.y)
        return -1;

    const int width = 2 * (bottomRight.x - topLeft.x);
    const int height = 2 * (bottomRight.y - topLeft.y);
    const int offsetX = 2 * (x - topLeft.x) + 1;
    const int offsetY = 2 * (y - topLeft.y) + 1;
    const int u = (topLeft.u * (width - offsetX) + bottomRight.u * offsetX) / width;
    const int v = (topLeft.v * (height - offsetY) + bottomRight.v * offsetY) / height;
    return v * kXenosQuadUploadSide + u;
}

typedef struct {
    uint32_t hash;
    unsigned backOnly, frontOnly, lowOverlap, highOverlap;
} BlendExpectation;

static BlendExpectation expectedBatch(const XenosQuadBlendedBatchUpload *upload) {
    const XenosQuadGeometryUpload *back = &upload->batch.layers[0].payload;
    const XenosQuadGeometryUpload *front = &upload->batch.layers[1].payload;
    const uint8_t clearRGB[] = {0x4c, 0xb2, 0x37};
    BlendExpectation expected = {XENOS_QUAD_HASH_INITIAL, 0, 0, 0, 0};
    for (int y = 0; y < kXenosQuadTargetSide; ++y) {
        for (int x = 0; x < kXenosQuadTargetSide; ++x) {
            const int backIndex = sampleIndex(&back->geometry, x, y);
            const int frontIndex = sampleIndex(&front->geometry, x, y);
            if (backIndex < 0 && frontIndex < 0)
                continue;
            const uint8_t *dst = backIndex >= 0 ? back->rgb + 3 * backIndex : clearRGB;
            const uint8_t *src = frontIndex >= 0 ? front->rgb + 3 * frontIndex : dst;
            const unsigned alpha = frontIndex >= 0 ? upload->frontAlpha[frontIndex] : 0;
            if (backIndex < 0)
                ++expected.frontOnly;
            else if (frontIndex < 0)
                ++expected.backOnly;
            else if (alpha == kLowAlpha)
                ++expected.lowOverlap;
            else if (alpha == kHighAlpha)
                ++expected.highOverlap;
            for (unsigned channel = 0; channel < 3; ++channel) {
                // Equivalent to interpolation with integer round-to-nearest.
                const int weighted = dst[channel] * 255 + (int)alpha * (src[channel] - dst[channel]);
                expected.hash = xenosQuadHashByte(expected.hash, (uint8_t)((weighted + 127) / 255));
            }
        }
    }
    return expected;
}

static UInt32 quadSourceOver(UInt32 source, UInt32 destination) {
  // The clear color and the first texture both have opaque alpha.
  const UInt32 alpha = source >> 24;
  UInt32 result = 0xff000000U;
  for (unsigned shift = 0; shift < 24; shift += 8) {
    const UInt32 src = (source >> shift) & 0xff;
    const UInt32 dst = (destination >> shift) & 0xff;
    result |= ((alpha * src + (255 - alpha) * dst + 127) / 255) << shift;
  }
  return result;
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

bool quadSampleAt(const QuadLayer &layer, UInt32 x, UInt32 y,
                                      UInt32 &u, UInt32 &v) {
  if (layer.rotated) {
    unsigned texelU = 0, texelV = 0;
    if (!xenosQuadRotatedPixel(&layer.rotation, x, y, &texelU, &texelV))
      return false;
    u = texelU;
    v = texelV;
    return true;
  }
  const XenosQuadMapping &m = layer.mapping;
  if (x < m.x0 || x >= m.x1 || y < m.y0 || y >= m.y1)
    return false;
  u = xenosQuadPointSample(m.u0, m.u1, x - m.x0, m.scaleX);
  v = xenosQuadPointSample(m.v0, m.v1, y - m.y0, m.scaleY);
  return true;
}

UInt32 quadLayerTexel(UInt32 layer, UInt32 u, UInt32 v) {
  const UInt32 index = v * kQuadSide + u;
  const UInt8 *rgb = _quadLayers[layer].payload.rgb + 3 * index;
  const UInt32 alpha = layer == 1 && _quadGeometryMode == kXenosQuadUploadBlendedBatch
      ? _quadFrontAlpha[index] : 255;
  return (alpha << 24) | (UInt32(rgb[0]) << 16) | (UInt32(rgb[1]) << 8) | rgb[2];
}


int main() {
    static_assert(sizeof(XenosQuadBatchUpload) == 1570, "Legacy batch ABI");
    static_assert(sizeof(XenosQuadBlendedBatchUpload) == 1826, "Blended batch ABI");
    const UInt32 clear = 0xff4cb237U;
    for (uint32_t seed : {0U, 1U, 67U, 0xffffffffU}) {
        XenosQuadBlendedBatchUpload upload;
        makeBatch(&upload, seed);
        memcpy(_quadFrontAlpha, upload.frontAlpha, sizeof(_quadFrontAlpha));
        for (unsigned layer = 0; layer < 2; ++layer) {
            _quadLayers[layer].payload = upload.batch.layers[layer].payload;
            assert(xenosQuadDecodeGeometry(&_quadLayers[layer].payload.geometry, &_quadLayers[layer].mapping));
        }

        const BlendExpectation reference = expectedBatch(&upload);
        UInt32 hash = XENOS_QUAD_HASH_INITIAL;
        unsigned backOnly = 0, frontOnly = 0, lowOverlap = 0, highOverlap = 0;
        unsigned detectsMissingFront = 0, detectsMissingBack = 0, detectsDisabledBlend = 0;
        for (unsigned y = 0; y < 32; ++y) {
            for (unsigned x = 0; x < 32; ++x) {
                UInt32 u = 0, v = 0;
                const bool first = quadSampleAt(_quadLayers[0], x, y, u, v);
                assert(first == (sampleIndex(&_quadLayers[0].payload.geometry, x, y) >= 0));
                const UInt32 destination = first ? quadLayerTexel(0, u, v) : clear;
                const bool second = quadSampleAt(_quadLayers[1], x, y, u, v);
                assert(second == (sampleIndex(&_quadLayers[1].payload.geometry, x, y) >= 0));
                if (!first && !second)
                    continue;
                const UInt32 source = second ? quadLayerTexel(1, u, v) : destination;
                const UInt32 expected = second ? quadSourceOver(source, destination) : destination;
                assert(expected >> 24 == 255);
                backOnly += first && !second;
                frontOnly += second && !first;
                lowOverlap += first && second && source >> 24 == kLowAlpha;
                highOverlap += first && second && source >> 24 == kHighAlpha;
                if (first && second) {
                    detectsMissingFront += quadMaxChannelDelta(expected, destination) > kXenosQuadBlendTolerance;
                    detectsMissingBack += quadMaxChannelDelta(expected, quadSourceOver(source, clear)) > kXenosQuadBlendTolerance;
                    detectsDisabledBlend += quadMaxChannelDelta(expected, 0xff000000U | source) > kXenosQuadBlendTolerance;
                }
                for (int shift = 16; shift >= 0; shift -= 8)
                    hash = xenosQuadHashByte(hash, (expected >> shift) & 0xff);
            }
        }
        assert(quadMaxChannelDelta(0xff010201U, 0xff020102U) == 1);
        assert(quadMaxChannelDelta(0xff080001U, 0xff020102U) == 6);
        assert(quadSourceOver(0x00243b66U, clear) == clear);
        assert(quadSourceOver(0xff243b66U, clear) == 0xff243b66U);
        assert(hash == reference.hash && backOnly == reference.backOnly && frontOnly == reference.frontOnly &&
               lowOverlap == reference.lowOverlap && highOverlap == reference.highOverlap);
        assert(backOnly && frontOnly && lowOverlap && highOverlap);
        const unsigned overlap = lowOverlap + highOverlap;
        assert(detectsMissingFront == overlap && detectsMissingBack == overlap && detectsDisabledBlend == overlap);
        printf("seed=%u back_only=%u front_only=%u overlap_alpha64=%u overlap_alpha192=%u reference_rgb_hash=%08x\n",
               seed, backOnly, frontOnly, lowOverlap, highOverlap, hash);
    }
}
