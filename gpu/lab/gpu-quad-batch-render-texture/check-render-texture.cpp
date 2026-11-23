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
static const UInt32 _quadGeometryMode = kXenosQuadUploadRenderTextureBatch, kQuadSide = kXenosQuadUploadSide;
static XenosQuadScissor _quadFrontMask[kXenosQuadStencilRects];
static XenosQuadScissor _quadFrontScissor;
static unsigned splitX;
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
    unsigned scissorRejectOnly, stencilRejectOnly, bothReject, depthReject;
    unsigned frontOnly, lowOverlap, highOverlap;
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

static ClipExpectation expectedBatch(const XenosQuadDepthBatchUpload *depth) {
    const XenosQuadScissorStencilBatchUpload *batch = &depth->clip;
    const XenosQuadBlendedBatchUpload *upload = &batch->batch.upload;
    const XenosQuadGeometryUpload *back = &upload->batch.layers[0].payload;
    const XenosQuadGeometryUpload *front = &upload->batch.layers[1].payload;
    const uint8_t clearRGB[] = {0x4c, 0xb2, 0x37};
    ClipExpectation expected = {XENOS_QUAD_HASH_INITIAL, 0, 0, 0, 0, 0, 0, 0};
    const int frontZ = 2 * depth->depthSplitX + 16;
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
                else if (backIndex >= 0 && frontZ >= 2 * x + 17) {
                    // Depths measured in units of 1/128 at the pixel center.
                    ++expected.depthReject;
                    frontIndex = -1;
                }
            }
            const uint8_t *dst = backIndex >= 0 ? back->rgb + 3 * backIndex : clearRGB;
            const uint8_t *src = frontIndex >= 0 ? front->rgb + 3 * frontIndex : dst;
            const unsigned alpha = frontIndex >= 0 ? upload->frontAlpha[frontIndex] : 0;
            if (backIndex < 0 && frontIndex >= 0)
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

static void packQuadDepth(const XenosQuadGeometry &geometry, UInt32 *positions,
                           bool rotated, bool opaque, UInt32 splitX) {
  // The opaque surface ramps with screen X while the translucent quad lies
  // at Z=(splitX+8)/64. Pixel centers never sit exactly on the split.
  const UInt32 positionScale = rotated ? 1 : kXenosQuadQuarter;
  for (UInt32 index = 0; index < 4; ++index) {
    const UInt32 xQuarter = opaque ? geometry.vertices[index].x * positionScale
                                   : splitX * kXenosQuadQuarter;
    positions[3 * index + 2] = coordinateWord(xQuarter + 8 * kXenosQuadQuarter, 8);
  }
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

bool quadUsesDepth();
bool quadUsesScissor() {
  return _quadGeometryMode == kXenosQuadUploadScissoredBatch ||
         _quadGeometryMode == kXenosQuadUploadScissorStencilBatch ||
         quadUsesDepth();
}

bool quadUsesStencil() {
  return _quadGeometryMode == kXenosQuadUploadStencilBatch ||
         _quadGeometryMode == kXenosQuadUploadScissorStencilBatch ||
         quadUsesDepth();
}

bool quadUsesDepth() {
  return _quadGeometryMode == kXenosQuadUploadDepthBatch ||
         _quadGeometryMode == kXenosQuadUploadRenderTextureBatch;
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

enum Mode { Correct, IgnoreScissor, IgnoreStencil, Union, IgnoreDepth, ReverseDepth };
struct Pixel { UInt32 color, alpha; bool back, front, scissorReject, stencilReject, depthReject; };

static Pixel samplePixel(Mode mode, unsigned x, unsigned y) {
    UInt32 u=0, v=0;
    const bool back=quadSampleAt(_quadLayers[0],x,y,u,v);
    const UInt32 dst=back ? quadLayerTexel(0,u,v) : 0xff4cb237U;
    const bool geometry=quadSampleAt(_quadLayers[1],x,y,u,v);
    const bool scissor=xenosQuadInScissor(&_quadFrontScissor,x,y);
    const bool stencil=quadFrontPassesStencil(x,y);
    const bool allow=mode==IgnoreScissor ? stencil : mode==IgnoreStencil ? scissor :
                     mode==Union ? scissor || stencil : scissor && stencil;
    const bool visible=geometry && allow;
    const bool depthReject=visible && back && mode!=IgnoreDepth &&
                           (mode==ReverseDepth ? x>=splitX : x<splitX);
    const bool front=visible && !depthReject;
    const UInt32 src=front ? quadLayerTexel(1,u,v) : dst;
    return {front ? quadSourceOver(src,dst) : dst, front ? src>>24 : 0, back, front,
            geometry && !scissor, geometry && !stencil, depthReject};
}

static ClipExpectation driverModel() {
    ClipExpectation result={XENOS_QUAD_HASH_INITIAL,0,0,0,0,0,0,0};
    for (unsigned y=0;y<32;++y) {
        for (unsigned x=0;x<32;++x) {
            const Pixel px=samplePixel(Correct,x,y);
            result.scissorRejectOnly += px.scissorReject && !px.stencilReject;
            result.stencilRejectOnly += px.stencilReject && !px.scissorReject;
            result.bothReject += px.scissorReject && px.stencilReject;
            result.depthReject += px.depthReject;
            result.frontOnly += px.front && !px.back;
            result.lowOverlap += px.back && px.alpha==kLowAlpha;
            result.highOverlap += px.back && px.alpha==kHighAlpha;
            result.hash=quadHashColor(result.hash,px.color);
        }
    }
    return result;
}


static void testPackedDepth() {
    XenosQuadGeometry geometry=xenosQuadMakeGeometry(2,3,18,19,0,0,16,16);
    for (bool rotated : {false,true}) {
        if (rotated)
            for (XenosQuadVertex &vertex : geometry.vertices)
                vertex.x=vertex.x*4+1;
        for (bool opaque : {false,true}) {
            UInt32 indices[12], uvs[8];
            packQuadGeometry(geometry,indices,uvs,rotated);
            packQuadDepth(geometry,indices,rotated,opaque,16);
            for (unsigned i=0;i<4;++i) {
                float depth;
                memcpy(&depth,&indices[3*i+2],sizeof(depth));
                const float x=rotated ? geometry.vertices[i].x/4.0f : geometry.vertices[i].x;
                assert(depth==((opaque ? x : 16.0f)+8.0f)/64.0f);
            }
        }
    }
}


static const UInt32 PAGE_SIZE=4096, CACHELINE_SIZE=128, kClearSide=kXenosQuadTargetSide;
static const UInt32 kClearPixels=kXenosQuadReadbackPixels, kSampleBorder=1, kSampleBackground=0xff213447U;
static UInt32 _quadSampleHash, _quadSampleMismatches, _quadSampleTexels, _quadSampleGuard, _quadSampleChecked;
static void flushMarkerCacheLine(volatile UInt32 *) {}
static void IOLog(const char *, ...) {}
void verifyQuadTexturePass(const volatile UInt32 *first, volatile UInt32 *sample) {
  for (UInt32 offset = 0; offset < PAGE_SIZE; offset += CACHELINE_SIZE)
    flushMarkerCacheLine(sample + offset / sizeof(UInt32));

  _quadSampleHash = XENOS_QUAD_HASH_INITIAL;
  for (UInt32 y = 0; y < kClearSide; ++y) {
    for (UInt32 x = 0; x < kClearSide; ++x) {
      const bool inside = x >= kSampleBorder && x < kClearSide - kSampleBorder &&
                          y >= kSampleBorder && y < kClearSide - kSampleBorder;
      const UInt32 observed = sample[resolveTileWord(x, y)];
      const UInt32 expected = inside ? first[resolveTileWord(kClearSide - 1 - x, y)] : kSampleBackground;
      const bool matched = observed == expected;
      if (!matched && !_quadSampleMismatches) {
        IOLog("XenosBatchRenderTexture: sample mismatch=(%lu,%lu) expected=%08lx observed=%08lx\n",
              x, y, expected, observed);
      }
      _quadSampleMismatches += !matched;
      _quadSampleTexels += matched && inside;
      _quadSampleGuard += matched && !inside;
      _quadSampleHash = quadHashColor(_quadSampleHash, observed);
    }
  }
  _quadSampleChecked = kClearPixels;
}


int main() {
    static_assert(sizeof(XenosQuadDepthBatchUpload)==1839, "Existing upload ABI");
    static_assert(kXenosQuadRenderTextureReadbackPixels==2048, "Two readbacks");
    XenosQuadDepthBatchUpload upload;
    const XenosQuadScissor mask[]={{13,11,17,17},{16,18,24,24}};
    const XenosQuadScissor rect={14,12,21,22};
    memcpy(upload.clip.batch.mask,mask,sizeof(mask));
    memcpy(_quadFrontMask,mask,sizeof(mask));
    _quadFrontScissor=upload.clip.frontScissor=rect;
    UInt32 first[kClearPixels], second[kClearPixels], incorrect[kClearPixels];
    const XenosQuadGeometry geometry=xenosQuadMakeGeometry(1,1,31,31,31,1,1,31);
    UInt32 positions[12],uvs[8];
    packQuadGeometry(geometry,positions,uvs,false);
    assert(uvs[0]==coordinateWord(31,5) && uvs[2]==coordinateWord(1,5));
    assert(uvs[1]==coordinateWord(31,5) && uvs[5]==coordinateWord(1,5));
    testPackedDepth();
    for (const uint32_t seed : {1U,67U,0xffffffffU}) {
        makeBatch(&upload.clip.batch.upload,seed);
        memcpy(_quadFrontAlpha,upload.clip.batch.upload.frontAlpha,sizeof(_quadFrontAlpha));
        for (unsigned i=0;i<2;++i) {
            _quadLayers[i].payload=upload.clip.batch.upload.batch.layers[i].payload;
            assert(xenosQuadDecodeGeometry(&_quadLayers[i].payload.geometry,&_quadLayers[i].mapping));
        }
        for (const unsigned split : {0U,16U,32U}) {
            splitX=upload.depthSplitX=split;
            const auto expected=expectedBatch(&upload), drv=driverModel();
            assert(expected.hash==drv.hash && expected.depthReject==drv.depthReject);
            for (unsigned y=0;y<32;++y)
                for (unsigned x=0;x<32;++x)
                    first[resolveTileWord(x,y)]=samplePixel(Correct,x,y).color;
            UInt32 sampleHash=XENOS_QUAD_HASH_INITIAL;
            for (unsigned y=0;y<32;++y) {
                for (unsigned x=0;x<32;++x) {
                    const bool inside=x>0 && x<31 && y>0 && y<31;
                    const UInt32 color=inside ? first[resolveTileWord(31-x,y)] : kSampleBackground;
                    second[resolveTileWord(x,y)]=color;
                    sampleHash=quadHashColor(sampleHash,color);
                    incorrect[resolveTileWord(x,y)]=inside ? first[resolveTileWord(x,y)] : kSampleBackground;
                }
            }
            _quadSampleMismatches=_quadSampleTexels=_quadSampleGuard=0;
            verifyQuadTexturePass(first,second);
            assert(_quadSampleChecked==1024 && _quadSampleMismatches==0 &&
                   _quadSampleTexels==900 && _quadSampleGuard==124 && _quadSampleHash==sampleHash);
            _quadSampleMismatches=_quadSampleTexels=_quadSampleGuard=0;
            verifyQuadTexturePass(first,incorrect);
            assert(_quadSampleMismatches>0 && _quadSampleGuard==124);
            second[resolveTileWord(0,5)]^=1;
            second[resolveTileWord(20,13)]^=1;
            _quadSampleMismatches=_quadSampleTexels=_quadSampleGuard=0;
            verifyQuadTexturePass(first,second);
            assert(_quadSampleMismatches==2 && _quadSampleGuard==123 && _quadSampleTexels==899);
            if (seed==1)
                printf("split=%u first_reference=%08x sample_reference=%08x depth_reject=%u overlap=%u\n",
                       split,expected.hash,sampleHash,expected.depthReject,expected.lowOverlap+expected.highOverlap);
        }
    }
    puts("Verified both pass models, mirrored UVs, and rejection of unmirrored output, stale texels and guard corruption.");
}
