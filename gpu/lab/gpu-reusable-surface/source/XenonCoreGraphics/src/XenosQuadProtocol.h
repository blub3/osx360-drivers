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
  kXenosQuadUploadRotated = 5,
  kXenosQuadUploadBatch = 6,
  kXenosQuadUploadBlendedBatch = 7,
  kXenosQuadUploadScissoredBatch = 8,
  kXenosQuadUploadStencilBatch = 9,
  kXenosQuadUploadScissorStencilBatch = 10,
  kXenosQuadUploadDepthBatch = 11,
  kXenosQuadUploadRenderTextureBatch = 12,
  kXenosQuadUploadBilinearBatch = 13,
  kXenosQuadUploadMipmapBatch = 14,
  kXenosQuadUploadTrilinearBatch = 15,
  kXenosQuadUploadAnisotropicBatch = 16,
  kXenosSurfaceCreate = 17,
  kXenosSurfaceClear = 18,
  kXenosSurfaceRead = 19,
  kXenosSurfaceRelease = 20,
  kXenosQuadBatchLayers = 2,
  kXenosQuadStencilRects = 2,
  kXenosQuadAxisAligned = 0,
  kXenosQuadRotated45 = 1,
  kXenosQuadUploadResultWords = 5,
  kXenosQuadUploadSide = 16,
  kXenosQuadTexturePixels = kXenosQuadUploadSide * kXenosQuadUploadSide,
  kXenosQuadUploadBytes = kXenosQuadTexturePixels * 3,
  kXenosQuadBlendTolerance = 1,
  kXenosQuadBilinearTolerance = 3,
  kXenosQuadTargetSide = 32,
  kXenosQuadQuarter = 4,
  kXenosQuadReadbackPixels = kXenosQuadTargetSide * kXenosQuadTargetSide,
  kXenosQuadRenderTextureReadbackPixels = 2 * kXenosQuadReadbackPixels,
  kXenosQuadMipmapReadbackPixels = 3 * kXenosQuadReadbackPixels,
  kXenosSurfaceSide = kXenosQuadTargetSide,
  kXenosSurfaceReadRows = 8,
  kXenosSurfaceReadPixels = kXenosSurfaceSide * kXenosSurfaceReadRows,
};

enum {
  kXenosQuadPending = 0,
  kXenosQuadPassed = 1,
  kXenosQuadPixelsFailed = 2,
  kXenosQuadUnavailable = 3,
  kXenosQuadTimedOut = 4,
};

// Read a band of 8 rows in row-major opaque ARGB8888. A clear's ticket
// selects the contents so a later clear cannot silently replace the readback.
typedef struct {
  uint32_t handle;
  uint32_t ticket;
  uint32_t firstRow;
} XenosSurfaceReadRequest;

typedef struct {
  uint32_t argb[kXenosSurfaceReadPixels];
} XenosSurfaceReadback;

// UVs name texel edges. Positions name pixel edges for selector 4
// and quarter-pixel coordinates for selector 5.
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
  uint8_t kind;
  XenosQuadGeometryUpload payload;
} XenosQuadBatchLayer;

typedef struct {
  // Draw in array order with opaque replacement.
  XenosQuadBatchLayer layers[kXenosQuadBatchLayers];
} XenosQuadBatchUpload;

typedef struct {
  XenosQuadBatchUpload batch;
  // Straight alpha for the second texture, in the same texel order as its RGB.
  uint8_t frontAlpha[kXenosQuadTexturePixels];
} XenosQuadBlendedBatchUpload;

// Left/top are inclusive. Right/bottom are exclusive.
typedef struct {
  uint8_t left, top, right, bottom;
} XenosQuadScissor;

typedef struct {
  XenosQuadBlendedBatchUpload upload;
  XenosQuadScissor frontScissor;
} XenosQuadScissoredBatchUpload;

typedef struct {
  XenosQuadBlendedBatchUpload upload;
  // The union of these target-space rectangles is stamped into stencil.
  XenosQuadScissor mask[kXenosQuadStencilRects];
} XenosQuadStencilBatchUpload;

typedef struct {
  XenosQuadStencilBatchUpload batch;
  XenosQuadScissor frontScissor;
} XenosQuadScissorStencilBatchUpload;

typedef struct {
  XenosQuadScissorStencilBatchUpload clip;
  // Where the translucent quad's constant depth crosses the opaque quad's ramp.
  uint8_t depthSplitX;
} XenosQuadDepthBatchUpload;

static inline int xenosQuadValidScissor(const XenosQuadScissor *rect) {
  return rect->left < rect->right && rect->top < rect->bottom &&
         rect->right <= kXenosQuadTargetSide && rect->bottom <= kXenosQuadTargetSide;
}

static inline int xenosQuadInScissor(const XenosQuadScissor *rect, unsigned x, unsigned y) {
  return x >= rect->left && x < rect->right && y >= rect->top && y < rect->bottom;
}

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

typedef struct {
  int direction; // +1 clockwise, -1 counterclockwise in screen coordinates.
  unsigned originX, originY;
  unsigned legU, legV;
  unsigned u0, v0, u1, v1;
  unsigned stepU, stepV; // Projection units per texel.
} XenosQuadRotation;

static inline int xenosQuadDecodeRotation(const XenosQuadGeometry *geometry,
                                           XenosQuadRotation *rotation) {
  const XenosQuadVertex *vertex = geometry->vertices;
  for (unsigned i = 0; i < 4; ++i) {
    if (vertex[i].x > kXenosQuadQuarter * kXenosQuadTargetSide ||
        vertex[i].y > kXenosQuadQuarter * kXenosQuadTargetSide ||
        vertex[i].u > kXenosQuadUploadSide || vertex[i].v > kXenosQuadUploadSide)
      return 0;
  }

  const XenosQuadVertex bl = vertex[0], br = vertex[1], tl = vertex[2], tr = vertex[3];
  const int dxU = tr.x - tl.x, dyU = tr.y - tl.y;
  const int dxV = bl.x - tl.x, dyV = bl.y - tl.y;
  const int direction = dyU > 0 ? 1 : -1;
  if (dxU <= 0 || dyV <= 0 || dyU != direction * dxU || dxV != -direction * dyV ||
      br.x != tr.x + dxV || br.y != tr.y + dyV ||
      bl.u != tl.u || bl.v != br.v || tr.u != br.u || tr.v != tl.v ||
      tl.u == br.u || tl.v == br.v)
    return 0;

  // Each sample center projects to a multiple of four quarter-pixels.
  // An odd origin and whole-pixel texel steps avoid every edge/UV tie.
  const unsigned uSpan = br.u > tl.u ? br.u - tl.u : tl.u - br.u;
  const unsigned vSpan = br.v > tl.v ? br.v - tl.v : tl.v - br.v;
  if (!((tl.x ^ tl.y) & 1) ||
      (2 * dxU) % uSpan || (2 * dyV) % vSpan)
    return 0;
  const unsigned stepU = 2 * dxU / uSpan, stepV = 2 * dyV / vSpan;
  if ((stepU != 8 && stepU != 16 && stepU != 32) ||
      (stepV != 8 && stepV != 16 && stepV != 32))
    return 0;

  const XenosQuadRotation decoded = {
    direction, tl.x, tl.y, (unsigned)dxU, (unsigned)dyV,
    tl.u, tl.v, br.u, br.v, stepU, stepV
  };
  *rotation = decoded;
  return 1;
}

static inline int xenosQuadRotatedPixel(const XenosQuadRotation *rotation,
                                        unsigned x, unsigned y, unsigned *u, unsigned *v) {
  // Unit vectors are (1, direction) and (-direction, 1), with length sqrt(2).
  const int dx = (int)(kXenosQuadQuarter * x + 2) - (int)rotation->originX;
  const int dy = (int)(kXenosQuadQuarter * y + 2) - (int)rotation->originY;
  const unsigned alongU = (unsigned)(dx + rotation->direction * dy);
  const unsigned alongV = (unsigned)(dy - rotation->direction * dx);
  if (alongU >= 2 * rotation->legU || alongV >= 2 * rotation->legV)
    return 0;
  *u = xenosQuadPointSample(rotation->u0, rotation->u1, alongU, rotation->stepU);
  *v = xenosQuadPointSample(rotation->v0, rotation->v1, alongV, rotation->stepV);
  return 1;
}

// FNV-1a over row-major RGB bytes in R, G, B order.
static inline uint32_t xenosQuadHashByte(uint32_t hash, uint8_t value) {
  return (hash ^ value) * 16777619U;
}

#define XENOS_QUAD_HASH_INITIAL 2166136261U

#endif
