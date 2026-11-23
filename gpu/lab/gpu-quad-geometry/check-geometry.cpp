#include <cstdio>
#include <cstring>
#include <cassert>
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

static void packQuadGeometry(const XenosQuadGeometry &geometry, UInt32 *positions, UInt32 *uvs) {
  for (UInt32 index = 0; index < 4; ++index) {
    const XenosQuadVertex &vertex = geometry.vertices[index];
    // Positions use clip space. The sampler uses a padded 32x32 texture.
    positions[3 * index] = coordinateWord(int(vertex.x) - 16, 4);
    positions[3 * index + 1] = coordinateWord(16 - int(vertex.y), 4);
    positions[3 * index + 2] = 0;
    uvs[2 * index] = coordinateWord(vertex.u, 5);
    uvs[2 * index + 1] = coordinateWord(vertex.v, 5);
  }
}

static unsigned sample(unsigned first, unsigned last, unsigned offset, unsigned extent) {
    const int start = (int)first, end = (int)last;
    const int denominator = 2 * (int)extent;
    return (unsigned)((start * denominator + (2 * (int)offset + 1) * (end - start)) / denominator);
}


static void equalsFloat(UInt32 observed, float expected) {
  UInt32 want;
  memcpy(&want,&expected,sizeof(want));
  assert(observed==want);
}
int main() {
  assert(sizeof(XenosQuadGeometryUpload)==784);
  for (int value=-16; value<=16; ++value)
    equalsFloat(coordinateWord(value,4),value/16.0f);
  for (int u=0; u<=16; ++u)
    equalsFloat(coordinateWord(u,5),u/32.0f);
  unsigned valid=0;
  for (unsigned first=0; first<=16; ++first) {
    for (unsigned last=0; last<=16; ++last) {
      for (unsigned width=1; width<=32; ++width) {
        XenosQuadMapping m;
        XenosQuadGeometry g=xenosQuadMakeGeometry(32-width,0,32,16,first,0,last,16);
        if (!xenosQuadDecodeGeometry(&g,&m))
          continue;
        ++valid;
        UInt32 positions[12],uv[8];
        packQuadGeometry(g,positions,uv);
        for (unsigned i=0; i<4; ++i) {
          equalsFloat(positions[3*i],((int)g.vertices[i].x-16)/16.0f);
          equalsFloat(positions[3*i+1],(16-(int)g.vertices[i].y)/16.0f);
          equalsFloat(uv[2*i],g.vertices[i].u/32.0f);
          equalsFloat(uv[2*i+1],g.vertices[i].v/32.0f);
        }
        for (unsigned offset=0; offset<width; ++offset) {
          const unsigned expected=sample(first,last,offset,width);
          assert(expected<16);
          assert(xenosQuadPointSample(first,last,offset,m.scaleX)==expected);
        }
        // Keep the strip topology and both UVs required at the RPC boundary.
        g.vertices[0].u ^= 1;
        assert(!xenosQuadDecodeGeometry(&g,&m));
      }
    }
  }
  XenosQuadMapping decoded;
  XenosQuadGeometry defaultQuad=xenosQuadMakeGeometry(5,7,29,19,14,2,2,14);
  assert(xenosQuadDecodeGeometry(&defaultQuad,&decoded));
  assert(decoded.scaleX==2 && decoded.scaleY==1);
  printf("Verified exact GPU coordinate encoding and point-sampling agreement for %u mapped axes, plus malformed strip rejection.\n",valid);
}
