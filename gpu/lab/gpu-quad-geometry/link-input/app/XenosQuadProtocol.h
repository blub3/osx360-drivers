#ifndef XenosQuadProtocol_h
#define XenosQuadProtocol_h

#include <stdint.h>

enum {
  kXenosQuadSubmit = 0,
  kXenosQuadStatus = 1,
  kXenosQuadTextureCount = 2,
  kXenosQuadResultWords = 4,
  kXenosQuadUpload = 2,
  kXenosQuadUploadStatus = 3,
  kXenosQuadUploadGeometry = 4,
  kXenosQuadUploadResultWords = 5,
  kXenosQuadUploadSide = 16,
  kXenosQuadUploadBytes = kXenosQuadUploadSide * kXenosQuadUploadSide * 3,
  kXenosQuadTargetSide = 32,
  kXenosQuadReadbackPixels = kXenosQuadTargetSide * kXenosQuadTargetSide,
};

enum {
  kXenosQuadPending = 0,
  kXenosQuadPassed = 1,
  kXenosQuadPixelsFailed = 2,
  kXenosQuadUnavailable = 3,
  kXenosQuadTimedOut = 4,
};

// Position and UV coordinates name pixel and texel edges, respectively.
typedef struct {
  uint8_t x, y, u, v;
} XenosQuadVertex;

typedef struct {
  XenosQuadVertex vertices[4]; // Bottom-left, bottom-right, top-left, top-right.
} XenosQuadGeometry;

typedef struct {
  XenosQuadGeometry geometry;
  uint8_t rgb[kXenosQuadUploadBytes];
} XenosQuadGeometryUpload;

typedef struct {
  uint8_t x0, y0, x1, y1;
  uint8_t u0, v0, u1, v1;
  uint8_t scaleX, scaleY;
} XenosQuadMapping;

static inline XenosQuadGeometry xenosQuadMakeGeometry(uint8_t x0, uint8_t y0,
                                                      uint8_t x1, uint8_t y1,
                                                      uint8_t u0, uint8_t v0,
                                                      uint8_t u1, uint8_t v1) {
  const XenosQuadGeometry geometry = {{
    {x0, y1, u0, v1}, {x1, y1, u1, v1}, {x0, y0, u0, v0}, {x1, y0, u1, v0},
  }};
  return geometry;
}

static inline int xenosQuadDecodeGeometry(const XenosQuadGeometry *geometry,
                                           XenosQuadMapping *mapping) {
  const XenosQuadVertex *vertices = geometry->vertices;
  const XenosQuadVertex tl = vertices[2], br = vertices[1];
  const XenosQuadVertex bl = vertices[0], tr = vertices[3];
  if (tl.x >= br.x || tl.y >= br.y ||
      br.x > kXenosQuadTargetSide || br.y > kXenosQuadTargetSide ||
      tl.u > kXenosQuadUploadSide || br.u > kXenosQuadUploadSide ||
      tl.v > kXenosQuadUploadSide || br.v > kXenosQuadUploadSide ||
      tl.u == br.u || tl.v == br.v ||
      bl.x != tl.x || bl.y != br.y || tr.x != br.x || tr.y != tl.y ||
      bl.u != tl.u || bl.v != br.v || tr.u != br.u || tr.v != tl.v)
    return 0;

  const unsigned width = br.x - tl.x, height = br.y - tl.y;
  const unsigned uSpan = br.u > tl.u ? br.u - tl.u : tl.u - br.u;
  const unsigned vSpan = br.v > tl.v ? br.v - tl.v : tl.v - br.v;
  if (width % uSpan || height % vSpan)
    return 0;
  const unsigned scaleX = width / uSpan, scaleY = height / vSpan;
  if ((scaleX != 1 && scaleX != 2 && scaleX != 4) ||
      (scaleY != 1 && scaleY != 2 && scaleY != 4))
    return 0;

  const XenosQuadMapping decoded = {
    tl.x, tl.y, br.x, br.y, tl.u, tl.v, br.u, br.v, (uint8_t)scaleX, (uint8_t)scaleY
  };
  *mapping = decoded;
  return 1;
}

static inline unsigned xenosQuadPointSample(unsigned first, unsigned last,
                                             unsigned offset, unsigned scale) {
  const unsigned index = offset / scale;
  return first < last ? first + index : first - 1 - index;
}

// FNV-1a over row-major RGB bytes in R, G, B order.
static inline uint32_t xenosQuadHashByte(uint32_t hash, uint8_t value) {
  return (hash ^ value) * 16777619U;
}

#define XENOS_QUAD_HASH_INITIAL 2166136261U

#endif
