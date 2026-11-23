#include <cassert>
#include <cstdio>
#include "XenosQuadProtocol.h"
typedef uint8_t UInt8;
typedef uint32_t UInt32;
static const UInt32 kQuadSide = kXenosQuadUploadSide;
static UInt32 quadTexel(const UInt8 *rgb, UInt32 x, UInt32 y) {
  const UInt8 *texel = rgb + (y * kQuadSide + x) * 3;
  return 0xff000000U | (UInt32(texel[0]) << 16) | (UInt32(texel[1]) << 8) | texel[2];
}
static UInt32 resolveTileWord(UInt32 x, UInt32 y) {
  const UInt32 pipe = (x >> 3) ^ ((y & 8) >> 2);
  const UInt32 rows = (y & 30) << 5;
  const UInt32 withinPipe = ((x & 4) << 1) | ((y & 1) << 2) | (x & 3);
  return rows | (pipe << 4) | withinPipe;
}
static void generateRGB(uint8_t *rgb, uint32_t seed) {
    for (unsigned i = 0; i < kXenosQuadUploadBytes; ++i) {
        seed = seed * 1664525U + 1013904223U;
        rgb[i] = (uint8_t)(seed >> 24);
    }
}

static UInt32 verify(UInt32 seed) {
  UInt8 rgb[kXenosQuadUploadBytes];
  generateRGB(rgb,seed);
  UInt32 tiled[32*32];
  bool seen[32*32]{};
  for (UInt32 y=0; y<32; ++y)
    for (UInt32 x=0; x<32; ++x) {
      UInt32 offset=resolveTileWord(x,y);
      assert(offset<1024 && !seen[offset]);
      seen[offset]=true;
      tiled[offset]=quadTexel(rgb,x%kQuadSide,y%kQuadSide);
    }
  UInt32 hash=XENOS_QUAD_HASH_INITIAL;
  for (UInt32 y=0; y<kQuadSide; ++y)
    for (UInt32 x=0; x<kQuadSide; ++x) {
      const UInt32 word=tiled[resolveTileWord(x,y)];
      assert((word>>24)==255);
      hash=xenosQuadHashByte(hash,(word>>16)&255);
      hash=xenosQuadHashByte(hash,(word>>8)&255);
      hash=xenosQuadHashByte(hash,word&255);
    }
  return hash;
}
int main() {

  assert(verify(0U) == 0x67ac8e62U);
  assert(verify(1U) == 0xf3e5eee0U);
  assert(verify(2026U) == 0x4c261c13U);
  assert(verify(4294967295U) == 0xbdae0a8bU);
  puts("Texture packing, tiled offsets, RGB channel order, and expected hashes verified for four seeds.");
}