#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include "XenosQuadProtocol.h"
typedef uint32_t UInt32;
struct QuadLayer {
    XenosQuadMapping mapping;
    XenosQuadRotation rotation;
    bool rotated;
};
static void generateRGB(uint8_t *rgb, uint32_t seed) {
    for (unsigned i = 0; i < kXenosQuadUploadBytes; ++i) {
        seed = seed * 1664525U + 1013904223U;
        rgb[i] = (uint8_t)(seed >> 24);
    }
}

static void makeBatch(XenosQuadBatchUpload *batch, const char *mode, uint32_t seed, int reverse) {
    XenosQuadBatchLayer *first = &batch->layers[reverse ? 1 : 0];
    XenosQuadBatchLayer *second = &batch->layers[reverse ? 0 : 1];
    first->kind = kXenosQuadAxisAligned;
    second->kind = kXenosQuadAxisAligned;
    first->payload.geometry = xenosQuadMakeGeometry(2, 3, 18, 19, 0, 0, 16, 16);
    second->payload.geometry = xenosQuadMakeGeometry(11, 9, 29, 29, 12, 2, 3, 12);
    if (strcmp(mode, "disjoint") == 0) {
        first->payload.geometry = xenosQuadMakeGeometry(1, 2, 13, 14, 1, 0, 13, 12);
        second->payload.geometry = xenosQuadMakeGeometry(18, 17, 30, 29, 13, 2, 1, 14);
    } else if (strcmp(mode, "rotated") == 0) {
        const XenosQuadGeometry rotated = {{
            {17, 52, 14, 13}, {65, 100, 2, 13}, {57, 12, 14, 3}, {105, 60, 2, 3},
        }};
        second->kind = kXenosQuadRotated45;
        second->payload.geometry = rotated;
    }
    generateRGB(first->payload.rgb, seed);
    generateRGB(second->payload.rgb, seed ^ 0x9e3779b9U);
}

// Invert the geometry at the pixel center independently of the driver's
// axis-aligned and 45-degree point-sample calculations.
static const uint8_t *sampleLayer(const XenosQuadBatchLayer *layer, int x, int y) {
    const XenosQuadVertex *vertices = layer->payload.geometry.vertices;
    const XenosQuadVertex tl = vertices[2], tr = vertices[3], bl = vertices[0];
    const int quarter = kXenosQuadQuarter;
    const int scale = layer->kind == kXenosQuadRotated45 ? 1 : quarter;
    const int ux = scale * (tr.x - tl.x), uy = scale * (tr.y - tl.y);
    const int vx = scale * (bl.x - tl.x), vy = scale * (bl.y - tl.y);
    const int det = ux * vy - uy * vx;
    const int dx = quarter * x + quarter / 2 - scale * tl.x;
    const int dy = quarter * y + quarter / 2 - scale * tl.y;
    const int a = dx * vy - dy * vx, b = ux * dy - uy * dx;
    if (a < 0 || b < 0 || a >= det || b >= det)
        return NULL;
    const unsigned u = (tl.u * det + a * (tr.u - tl.u)) / det;
    const unsigned v = (tl.v * det + b * (bl.v - tl.v)) / det;
    return layer->payload.rgb + 3 * (v * kXenosQuadUploadSide + u);
}

typedef struct {
    uint32_t hash;
    unsigned firstOnly, secondOnly, overlap;
} BatchExpectation;

static BatchExpectation expectedBatch(const XenosQuadBatchUpload *batch) {
    BatchExpectation expected = {XENOS_QUAD_HASH_INITIAL, 0, 0, 0};
    for (int y = 0; y < kXenosQuadTargetSide; ++y) {
        for (int x = 0; x < kXenosQuadTargetSide; ++x) {
            const uint8_t *first = sampleLayer(&batch->layers[0], x, y);
            const uint8_t *second = sampleLayer(&batch->layers[1], x, y);
            if (first && second)
                ++expected.overlap;
            else if (first)
                ++expected.firstOnly;
            else if (second)
                ++expected.secondOnly;
            const uint8_t *visible = second ? second : first;
            if (visible) {
                for (unsigned channel = 0; channel < 3; ++channel)
                    expected.hash = xenosQuadHashByte(expected.hash, visible[channel]);
            }
        }
    }
    return expected;
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


static BatchExpectation driverResult(const XenosQuadBatchUpload &batch) {
  QuadLayer layers[2] = {};
  for (unsigned i = 0; i < 2; ++i) {
    const XenosQuadBatchLayer &layer = batch.layers[i];
    layers[i].rotated = layer.kind == kXenosQuadRotated45;
    assert(layers[i].rotated ? xenosQuadDecodeRotation(&layer.payload.geometry, &layers[i].rotation) :
                               xenosQuadDecodeGeometry(&layer.payload.geometry, &layers[i].mapping));
  }

  BatchExpectation result = {XENOS_QUAD_HASH_INITIAL, 0, 0, 0};
  for (unsigned y = 0; y < 32; ++y)
    for (unsigned x = 0; x < 32; ++x) {
      unsigned u = 0, v = 0;
      const bool first = quadSampleAt(layers[0], x, y, u, v);
      const bool second = quadSampleAt(layers[1], x, y, u, v);
      const auto *directFirst = sampleLayer(&batch.layers[0], x, y);
      const auto *directSecond = sampleLayer(&batch.layers[1], x, y);
      assert(first == (directFirst != nullptr) && second == (directSecond != nullptr));
      result.firstOnly += first && !second;
      result.secondOnly += second && !first;
      result.overlap += first && second;
      if (!first && !second)
        continue;
      assert(u < 16 && v < 16);
      const auto *rgb = batch.layers[second ? 1 : 0].payload.rgb + 3 * (v * 16 + u);
      assert(rgb == (second ? directSecond : directFirst));
      for (unsigned channel = 0; channel < 3; ++channel)
        result.hash = xenosQuadHashByte(result.hash, rgb[channel]);
    }
  return result;
}

int main() {
  static_assert(sizeof(XenosQuadBatchUpload) == 1570, "Tiger upload size");
  for (const char *mode : {"overlap", "disjoint", "rotated"}) {
    uint32_t forward = 0;
    for (int reverse : {0, 1}) {
      XenosQuadBatchUpload batch;
      makeBatch(&batch, mode, 1, reverse);
      const auto expected = expectedBatch(&batch);
      const auto got = driverResult(batch);
      assert(expected.hash == got.hash && expected.firstOnly == got.firstOnly &&
             expected.secondOnly == got.secondOnly && expected.overlap == got.overlap);
      assert(got.firstOnly && got.secondOnly);
      assert((got.overlap == 0) == (strcmp(mode, "disjoint") == 0));
      assert(!reverse || ((forward == got.hash) == (got.overlap == 0)));
      forward = got.hash;
      printf("%s reverse=%d first_only=%u second_only=%u overlap=%u expected_hash=%08x\n",
             mode, reverse, got.firstOnly, got.secondOnly, got.overlap, got.hash);
    }
  }
}
