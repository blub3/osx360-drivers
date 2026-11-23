#include <cassert>
#include <cstdio>
#include <cstring>
#include "XenosQuadProtocol.h"
typedef uint32_t UInt32;
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

static void generateRGB(uint8_t *rgb, uint32_t seed) {
    for (unsigned i = 0; i < kXenosQuadUploadBytes; ++i) {
        seed = seed * 1664525U + 1013904223U;
        rgb[i] = (uint8_t)(seed >> 24);
    }
}

static int makeRotation(XenosQuadGeometry *geometry, const uint8_t *shape,
                        const uint8_t *uv, int direction) {
    const int ox = 4 * shape[0] + 1, oy = 4 * shape[1];
    const int ux = 4 * shape[2], uy = direction * ux;
    const int vy = 4 * shape[3], vx = -direction * vy;
    const int xs[] = {ox + vx, ox + ux + vx, ox, ox + ux};
    const int ys[] = {oy + vy, oy + uy + vy, oy, oy + uy};
    for (unsigned i = 0; i < 4; ++i) {
        if (xs[i] < 0 || ys[i] < 0 ||
            xs[i] > 4 * kXenosQuadTargetSide || ys[i] > 4 * kXenosQuadTargetSide)
            return 0;
    }
    const XenosQuadGeometry result = {{
        {(uint8_t)xs[0], (uint8_t)ys[0], uv[0], uv[3]},
        {(uint8_t)xs[1], (uint8_t)ys[1], uv[2], uv[3]},
        {(uint8_t)xs[2], (uint8_t)ys[2], uv[0], uv[1]},
        {(uint8_t)xs[3], (uint8_t)ys[3], uv[2], uv[1]},
    }};
    *geometry = result;
    return 1;
}

// Use the parallelogram determinant to invert the uploaded vertices at
// each pixel center. This is independent of the driver's 45-degree projection.
static uint32_t expectedHash(const XenosQuadGeometryUpload *upload, unsigned *covered) {
    const XenosQuadVertex *vertices = upload->geometry.vertices;
    const XenosQuadVertex tl = vertices[2], tr = vertices[3], bl = vertices[0];
    const int ux = tr.x - tl.x, uy = tr.y - tl.y;
    const int vx = bl.x - tl.x, vy = bl.y - tl.y;
    const int det = ux * vy - uy * vx;
    uint32_t hash = XENOS_QUAD_HASH_INITIAL;
    *covered = 0;
    for (int y = 0; y < kXenosQuadTargetSide; ++y) {
        for (int x = 0; x < kXenosQuadTargetSide; ++x) {
            const int dx = 4 * x + 2 - tl.x, dy = 4 * y + 2 - tl.y;
            const int a = dx * vy - dy * vx, b = ux * dy - uy * dx;
            if (a < 0 || b < 0 || a >= det || b >= det)
                continue;
            const unsigned u = (tl.u * det + a * ((int)tr.u - tl.u)) / det;
            const unsigned v = (tl.v * det + b * ((int)bl.v - tl.v)) / det;
            const uint8_t *rgb = upload->rgb + 3 * (v * kXenosQuadUploadSide + u);
            for (unsigned channel = 0; channel < 3; ++channel)
                hash = xenosQuadHashByte(hash, rgb[channel]);
            ++*covered;
        }
    }
    return hash;
}


static void equalsFloat(UInt32 observed, float expected) {
  UInt32 want;
  memcpy(&want,&expected,sizeof(want));
  assert(observed==want);
}

static unsigned verify(const XenosQuadGeometryUpload &upload, const XenosQuadRotation &r) {
  const XenosQuadVertex *v=upload.geometry.vertices;
  UInt32 pos[12],uv[8];
  packQuadGeometry(upload.geometry,pos,uv,true);
  for (unsigned i=0; i<4; ++i) {
    equalsFloat(pos[3*i], (int(v[i].x)-64)/64.0f);
    equalsFloat(pos[3*i+1], (64-int(v[i].y))/64.0f);
    equalsFloat(uv[2*i],v[i].u/32.0f);
    equalsFloat(uv[2*i+1],v[i].v/32.0f);
  }
  unsigned covered=0;
  UInt32 actual=XENOS_QUAD_HASH_INITIAL;
  for (unsigned y=0;y<32;++y)
    for(unsigned x=0;x<32;++x) {
      unsigned u=0,vTexel=0;
      if (!xenosQuadRotatedPixel(&r,x,y,&u,&vTexel))
        continue;
      ++covered;
      assert(u<16 && vTexel<16);
      for(unsigned c=0;c<3;++c)
        actual=xenosQuadHashByte(actual,upload.rgb[(vTexel*16+u)*3+c]);
    }
  unsigned expectedCovered=0;
  const UInt32 want=expectedHash(&upload,&expectedCovered);
  assert(covered==expectedCovered && covered>0 && actual==want);
  assert(covered==2*(r.legU/4)*(r.legV/4));
  return covered;
}

int main() {
  assert(sizeof(XenosQuadGeometryUpload)==784);
  XenosQuadGeometryUpload upload;
  generateRGB(upload.rgb,2026);
  unsigned count=0;
  for(int turn=-1;turn<=1;turn+=2)
    for(unsigned x=0;x<32;x+=3)
      for(unsigned y=0;y<32;y+=3)
        for(unsigned uLeg=2;uLeg<=16;uLeg+=2)
          for(unsigned vLeg=2;vLeg<=16;vLeg+=2) {
            const uint8_t shape[]={uint8_t(x),uint8_t(y),uint8_t(uLeg),uint8_t(vLeg)};
            const uint8_t uv[]={uint8_t(uLeg/2+1),14,1,uint8_t(14-vLeg/2)};
            if(!makeRotation(&upload.geometry,shape,uv,turn))
              continue;
            XenosQuadRotation r;
            assert(xenosQuadDecodeRotation(&upload.geometry,&r));
            assert(r.direction==turn);
            verify(upload,r);
            ++count;
            upload.geometry.vertices[1].x+=1;
            assert(!xenosQuadDecodeRotation(&upload.geometry,&r));
          }
  const uint8_t defaultShape[]={14,3,12,10}, defaultUV[]={14,3,2,13};
  assert(makeRotation(&upload.geometry,defaultShape,defaultUV,1));
  XenosQuadRotation r;
  assert(xenosQuadDecodeRotation(&upload.geometry,&r));
  generateRGB(upload.rgb,1);
  unsigned covered;
  const UInt32 defaultHash=expectedHash(&upload,&covered);
  printf("Default clockwise image expects %u covered pixels and checksum %08x.\n",covered,defaultHash);
  printf("Verified GPU position packing, both directions, coverage and UV checksum for %u rotations.\n",count);
}
