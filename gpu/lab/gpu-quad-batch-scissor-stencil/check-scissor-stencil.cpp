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
static const UInt32 _quadGeometryMode = kXenosQuadUploadScissorStencilBatch, kQuadSide = kXenosQuadUploadSide;
static XenosQuadScissor _quadFrontMask[kXenosQuadStencilRects];
static XenosQuadScissor _quadFrontScissor;
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
    unsigned scissorRejectOnly, stencilRejectOnly, bothReject, frontOnly, lowOverlap, highOverlap;
} ClipExpectation;

static int pixelInRect(const XenosQuadScissor *rect, int x, int y) {
    // Compare pixel centers with the edges in the uploaded rectangle.
    const int centerX = 2 * x + 1, centerY = 2 * y + 1;
    return 2 * rect->left < centerX && centerX < 2 * rect->right &&
           2 * rect->top < centerY && centerY < 2 * rect->bottom;
}

static int pixelInMask(const XenosQuadStencilBatchUpload *batch, int x, int y) {
    for (unsigned i = 0; i < kXenosQuadStencilRects; ++i) {
        if (pixelInRect(&batch->mask[i], x, y))
            return 1;
    }
    return 0;
}

static ClipExpectation expectedBatch(const XenosQuadScissorStencilBatchUpload *batch) {
    const XenosQuadBlendedBatchUpload *upload = &batch->batch.upload;
    const XenosQuadGeometryUpload *back = &upload->batch.layers[0].payload;
    const XenosQuadGeometryUpload *front = &upload->batch.layers[1].payload;
    const uint8_t clearRGB[] = {0x4c, 0xb2, 0x37};
    ClipExpectation expected = {XENOS_QUAD_HASH_INITIAL, 0, 0, 0, 0, 0, 0};
    for (int y = 0; y < kXenosQuadTargetSide; ++y) {
        for (int x = 0; x < kXenosQuadTargetSide; ++x) {
            const int backIndex = sampleIndex(&back->geometry, x, y);
            int frontIndex = sampleIndex(&front->geometry, x, y);
            if (frontIndex >= 0) {
                const int scissor = pixelInRect(&batch->frontScissor, x, y);
                const int stencil = pixelInMask(&batch->batch, x, y);
                expected.scissorRejectOnly += !scissor && stencil;
                expected.stencilRejectOnly += scissor && !stencil;
                expected.bothReject += !scissor && !stencil;
                if (!scissor || !stencil)
                    frontIndex = -1;
            }
            if (backIndex < 0 && frontIndex < 0)
                continue;
            const uint8_t *dst = backIndex >= 0 ? back->rgb + 3 * backIndex : clearRGB;
            const uint8_t *src = frontIndex >= 0 ? front->rgb + 3 * frontIndex : dst;
            const unsigned alpha = frontIndex >= 0 ? upload->frontAlpha[frontIndex] : 0;
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

bool quadFrontPassesStencil(UInt32 x, UInt32 y) {
  for (UInt32 index = 0; index < kXenosQuadStencilRects; ++index) {
    if (xenosQuadInScissor(&_quadFrontMask[index], x, y))
      return true;
  }
  return false;
}

bool quadUsesScissor() {
  return _quadGeometryMode == kXenosQuadUploadScissoredBatch ||
         _quadGeometryMode == kXenosQuadUploadScissorStencilBatch;
}

bool quadUsesStencil() {
  return _quadGeometryMode == kXenosQuadUploadStencilBatch ||
         _quadGeometryMode == kXenosQuadUploadScissorStencilBatch;
}

bool quadUsesBlending() {
  return _quadGeometryMode == kXenosQuadUploadBlendedBatch ||
         quadUsesScissor() || quadUsesStencil();
}

UInt32 quadLayerTexel(UInt32 layer, UInt32 u, UInt32 v) {
  const UInt32 index = v * kQuadSide + u;
  const UInt8 *rgb = _quadLayers[layer].payload.rgb + 3 * index;
  const UInt32 alpha = layer == 1 && quadUsesBlending()
      ? _quadFrontAlpha[index] : 255;
  return (alpha << 24) | (UInt32(rgb[0]) << 16) | (UInt32(rgb[1]) << 8) | rgb[2];
}


enum Mode { Correct, IgnoreScissor, IgnoreStencil, Union };
struct Pixel { UInt32 color, alpha; bool back, front, scissorReject, stencilReject; };

static Pixel samplePixel(Mode mode, unsigned x, unsigned y) {
    UInt32 u=0, v=0;
    const bool back=quadSampleAt(_quadLayers[0],x,y,u,v);
    const UInt32 dst=back ? quadLayerTexel(0,u,v) : 0xff4cb237U;
    const bool geometry=quadSampleAt(_quadLayers[1],x,y,u,v);
    const bool scissor=xenosQuadInScissor(&_quadFrontScissor,x,y);
    const bool stencil=quadFrontPassesStencil(x,y);
    const bool allow=mode==Correct ? scissor && stencil : mode==IgnoreScissor ? stencil :
                     mode==IgnoreStencil ? scissor : scissor || stencil;
    const bool front=geometry && allow;
    const UInt32 src=front ? quadLayerTexel(1,u,v) : dst;
    return {front ? quadSourceOver(src,dst) : dst, front ? src>>24 : 0, back, front,
            geometry && !scissor, geometry && !stencil};
}

static ClipExpectation driverModel() {
    ClipExpectation result={XENOS_QUAD_HASH_INITIAL,0,0,0,0,0,0};
    for (unsigned y=0;y<32;++y) {
        for (unsigned x=0;x<32;++x) {
            const Pixel px=samplePixel(Correct,x,y);
            result.scissorRejectOnly += px.scissorReject && !px.stencilReject;
            result.stencilRejectOnly += px.stencilReject && !px.scissorReject;
            result.bothReject += px.scissorReject && px.stencilReject;
            result.frontOnly += px.front && !px.back;
            result.lowOverlap += px.back && px.alpha==kLowAlpha;
            result.highOverlap += px.back && px.alpha==kHighAlpha;
            if (px.back || px.front)
                for (int shift=16;shift>=0;shift-=8)
                    result.hash=xenosQuadHashByte(result.hash,(px.color>>shift)&0xff);
        }
    }
    return result;
}

int main() {
    static_assert(sizeof(XenosQuadBatchUpload)==1570, "Opaque batch ABI");
    static_assert(sizeof(XenosQuadScissoredBatchUpload)==1830, "Scissor ABI");
    static_assert(sizeof(XenosQuadStencilBatchUpload)==1834, "Stencil ABI");
    static_assert(sizeof(XenosQuadScissorStencilBatchUpload)==1838, "Combined ABI");
    const XenosQuadScissor masks[][2]={
        {{13,11,17,17},{16,18,24,24}}, {{0,0,32,32},{2,2,3,3}}, {{0,0,1,1},{31,31,32,32}},
        {{13,12,21,22},{17,16,25,26}}
    };
    const XenosQuadScissor scissors[]={{14,12,21,22},{0,0,32,32},{0,0,1,1},{16,18,17,19}};
    for (const uint32_t seed : {1U,67U,0xffffffffU}) {
        XenosQuadScissorStencilBatchUpload upload;
        makeBatch(&upload.batch.upload,seed);
        memcpy(_quadFrontAlpha,upload.batch.upload.frontAlpha,sizeof(_quadFrontAlpha));
        for (unsigned i=0;i<2;++i) {
            _quadLayers[i].payload=upload.batch.upload.batch.layers[i].payload;
            assert(xenosQuadDecodeGeometry(&_quadLayers[i].payload.geometry,&_quadLayers[i].mapping));
        }
        for (const auto &mask : masks) {
            memcpy(upload.batch.mask,mask,sizeof(upload.batch.mask));
            memcpy(_quadFrontMask,mask,sizeof(_quadFrontMask));
            for (const auto &rect : scissors) {
                _quadFrontScissor=upload.frontScissor=rect;
                const auto app=expectedBatch(&upload), drv=driverModel();
                assert(app.hash==drv.hash && app.scissorRejectOnly==drv.scissorRejectOnly &&
                       app.stencilRejectOnly==drv.stencilRejectOnly && app.bothReject==drv.bothReject &&
                       app.frontOnly==drv.frontOnly && app.lowOverlap==drv.lowOverlap &&
                       app.highOverlap==drv.highOverlap);
            }
        }
        memcpy(upload.batch.mask,masks[0],sizeof(upload.batch.mask));
        memcpy(_quadFrontMask,masks[0],sizeof(_quadFrontMask));
        _quadFrontScissor=upload.frontScissor=scissors[0];
        const auto app=expectedBatch(&upload);
        assert(app.scissorRejectOnly==37 && app.stencilRejectOnly==35 && app.bothReject==253 &&
               app.frontOnly==18 && app.lowOverlap==3 && app.highOverlap==14);
        for (const auto mode : {IgnoreScissor, IgnoreStencil, Union}) {
            unsigned mismatches=0;
            for (unsigned y=0;y<32;++y)
                for (unsigned x=0;x<32;++x)
                    mismatches+=quadMaxChannelDelta(samplePixel(Correct,x,y).color,
                                                   samplePixel(mode,x,y).color)>kXenosQuadBlendTolerance;
            assert(mismatches>0);
        }
        printf("seed=%u scissor_reject_only=%u stencil_reject_only=%u both_reject=%u front_only=%u overlap_alpha64=%u overlap_alpha192=%u reference_rgb_hash=%08x\n",
               seed,app.scissorRejectOnly,app.stencilRejectOnly,app.bothReject,app.frontOnly,app.lowOverlap,app.highOverlap,app.hash);
    }
}
