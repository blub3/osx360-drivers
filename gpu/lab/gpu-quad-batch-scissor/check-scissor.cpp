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
static const UInt32 _quadGeometryMode = kXenosQuadUploadScissoredBatch, kQuadSide = kXenosQuadUploadSide;
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
    unsigned clippedBack, clippedClear, frontOnly, lowOverlap, highOverlap;
} ScissorExpectation;

static ScissorExpectation expectedBatch(const XenosQuadScissoredBatchUpload *batch) {
    const XenosQuadGeometryUpload *back = &batch->upload.batch.layers[0].payload;
    const XenosQuadGeometryUpload *front = &batch->upload.batch.layers[1].payload;
    const XenosQuadScissor rect = batch->frontScissor;
    const uint8_t clearRGB[] = {0x4c, 0xb2, 0x37};
    ScissorExpectation expected = {XENOS_QUAD_HASH_INITIAL, 0, 0, 0, 0, 0};
    for (int y = 0; y < kXenosQuadTargetSide; ++y) {
        for (int x = 0; x < kXenosQuadTargetSide; ++x) {
            const int backIndex = sampleIndex(&back->geometry, x, y);
            int frontIndex = sampleIndex(&front->geometry, x, y);
            if (frontIndex >= 0 && (x < rect.left || y < rect.top || x >= rect.right || y >= rect.bottom)) {
                expected.clippedBack += backIndex >= 0;
                expected.clippedClear += backIndex < 0;
                frontIndex = -1;
            }
            if (backIndex < 0 && frontIndex < 0)
                continue;
            const uint8_t *dst = backIndex >= 0 ? back->rgb + 3 * backIndex : clearRGB;
            const uint8_t *src = frontIndex >= 0 ? front->rgb + 3 * frontIndex : dst;
            const unsigned alpha = frontIndex >= 0 ? batch->upload.frontAlpha[frontIndex] : 0;
            if (backIndex < 0)
                ++expected.frontOnly;
            else if (frontIndex >= 0 && alpha == kLowAlpha)
                ++expected.lowOverlap;
            else if (frontIndex >= 0 && alpha == kHighAlpha)
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

bool quadUsesBlending() {
  return _quadGeometryMode == kXenosQuadUploadBlendedBatch ||
         _quadGeometryMode == kXenosQuadUploadScissoredBatch;
}

UInt32 quadLayerTexel(UInt32 layer, UInt32 u, UInt32 v) {
  const UInt32 index = v * kQuadSide + u;
  const UInt8 *rgb = _quadLayers[layer].payload.rgb + 3 * index;
  const UInt32 alpha = layer == 1 && quadUsesBlending()
      ? _quadFrontAlpha[index] : 255;
  return (alpha << 24) | (UInt32(rgb[0]) << 16) | (UInt32(rgb[1]) << 8) | rgb[2];
}


struct Pixel {
    UInt32 color;
    bool back, frontGeometry, second, clipped;
    UInt32 alpha;
};

static Pixel calculatePixel(const XenosQuadScissor &rect, unsigned x, unsigned y) {
    UInt32 u = 0, v = 0;
    const bool back = quadSampleAt(_quadLayers[0], x, y, u, v);
    const UInt32 dst = back ? quadLayerTexel(0, u, v) : 0xff4cb237U;
    const bool frontGeometry = quadSampleAt(_quadLayers[1], x, y, u, v);
    const bool second = frontGeometry && xenosQuadInScissor(&rect, x, y);
    const UInt32 src = second ? quadLayerTexel(1, u, v) : dst;
    const Pixel pixel = {
        second ? quadSourceOver(src, dst) : dst, back, frontGeometry, second,
        frontGeometry && !second, second ? src >> 24 : 0
    };
    return pixel;
}

static ScissorExpectation referenceDriver(const XenosQuadScissor &rect) {
    ScissorExpectation result = {XENOS_QUAD_HASH_INITIAL, 0, 0, 0, 0, 0};
    for (unsigned y = 0; y < 32; ++y) {
        for (unsigned x = 0; x < 32; ++x) {
            const Pixel px = calculatePixel(rect, x, y);
            result.clippedBack += px.clipped && px.back;
            result.clippedClear += px.clipped && !px.back;
            result.frontOnly += px.second && !px.back;
            result.lowOverlap += px.back && px.alpha == kLowAlpha;
            result.highOverlap += px.back && px.alpha == kHighAlpha;
            if (px.back || px.second) {
                for (int shift = 16; shift >= 0; shift -= 8)
                    result.hash = xenosQuadHashByte(result.hash, (px.color >> shift) & 0xff);
            }
        }
    }
    return result;
}

static void assertReference(const XenosQuadScissoredBatchUpload &upload) {
    const auto expected = expectedBatch(&upload);
    const auto gpuModel = referenceDriver(upload.frontScissor);
    assert(expected.hash == gpuModel.hash && expected.clippedBack == gpuModel.clippedBack &&
           expected.clippedClear == gpuModel.clippedClear && expected.frontOnly == gpuModel.frontOnly &&
           expected.lowOverlap == gpuModel.lowOverlap && expected.highOverlap == gpuModel.highOverlap);
}

int main() {
    static_assert(sizeof(XenosQuadBlendedBatchUpload) == 1826, "Legacy blend ABI");
    static_assert(sizeof(XenosQuadScissoredBatchUpload) == 1830, "Scissor ABI");
    const XenosQuadScissor testRect = {13, 11, 24, 23};
    const XenosQuadScissor full = {0, 0, 32, 32};
    const XenosQuadScissor invalid[] = {{0, 0, 33, 32}, {13, 11, 13, 20}, {13, 11, 12, 23},
                                       {0, 0, 32, 33}, {13, 11, 15, 11}, {13, 11, 15, 10}};
    for (const auto &rect : invalid)
        assert(!xenosQuadValidScissor(&rect));
    const XenosQuadScissor fixtures[] = {testRect, full, {0, 0, 1, 1}, {17, 18, 18, 19},
                                        {14, 15, 32, 16}, {12, 8, 32, 32}};
    for (uint32_t seed : {1U, 67U, 0xffffffffU}) {
        XenosQuadScissoredBatchUpload upload;
        makeBatch(&upload.upload, seed);
        memcpy(_quadFrontAlpha, upload.upload.frontAlpha, sizeof(_quadFrontAlpha));
        for (unsigned layer = 0; layer < 2; ++layer) {
            _quadLayers[layer].payload = upload.upload.batch.layers[layer].payload;
            assert(xenosQuadDecodeGeometry(&_quadLayers[layer].payload.geometry, &_quadLayers[layer].mapping));
        }
        for (const auto &rect : fixtures) {
            assert(xenosQuadValidScissor(&rect));
            upload.frontScissor = rect;
            assertReference(upload);
        }
        upload.frontScissor = testRect;
        const ScissorExpectation expected = expectedBatch(&upload);
        assert(expected.clippedBack == 30 && expected.clippedClear == 198 && expected.frontOnly == 92 &&
               expected.lowOverlap == 10 && expected.highOverlap == 30);
        const XenosQuadScissor erroneous[] = {full, {12, 11, 24, 23}, {14, 11, 24, 23},
                                              {13, 10, 24, 23}, {13, 12, 24, 23},
                                              {13, 11, 23, 23}, {13, 11, 25, 23},
                                              {13, 11, 24, 22}, {13, 11, 24, 24}};
        for (const auto &rect : erroneous) {
            unsigned mismatches = 0;
            for (unsigned y = 0; y < 32; ++y)
                for (unsigned x = 0; x < 32; ++x)
                    mismatches += quadMaxChannelDelta(calculatePixel(testRect, x, y).color,
                                                      calculatePixel(rect, x, y).color) > kXenosQuadBlendTolerance;
            assert(mismatches > 0);
        }
        printf("seed=%u clipped_back=%u clipped_clear=%u front_only=%u overlap_alpha64=%u overlap_alpha192=%u reference_rgb_hash=%08x\n",
               seed, expected.clippedBack, expected.clippedClear, expected.frontOnly,
               expected.lowOverlap, expected.highOverlap, expected.hash);
    }
}
