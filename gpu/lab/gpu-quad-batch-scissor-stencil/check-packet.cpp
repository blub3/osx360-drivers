
#include <cassert>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
static uint32_t fui(float value) {
    uint32_t word;
    std::memcpy(&word, &value, sizeof(word));
    return word;
}
#include "adreno_pm4.xml.h"
#include "adreno_common.xml.h"
#include "a2xx.xml.h"
using UInt32 = uint32_t;
using UInt16 = uint16_t;
static const UInt32 CACHELINE_SIZE = 128;
static const UInt32 kQuadGeometryStride = 2 * CACHELINE_SIZE;
static const UInt32 kXenosQuadStencilRects = 2;
class XenosController {
public:
  std::vector<UInt32> words;
  void writeRing(UInt32 word) { words.push_back(word); }
  inline void writeRingRegister(UInt16 index, UInt32 data) {
    writeRingPacket0(index, 1);
    writeRing(data);
  }
  inline void writeRingPacket0(UInt16 index, UInt16 length) {
    writeRing(CP_TYPE0_PKT | ((length - 1) << 16) | (index & 0x7FFF));
  }

  void drawTextureLayer(UInt32 position, UInt32 uv, UInt32 texture) {
    assert(uv - position == 128 && texture == 0x8000);
    words.push_back(position);
  }
  void stampQuadStencil(UInt32 vertices, UInt32 texture, UInt32 colorMask);
};
void XenosController::stampQuadStencil(UInt32 vertices, UInt32 texture, UInt32 colorMask) {
  // Keep depth writes disabled. Both faces use the front stencil settings.
  const UInt32 depthAlways = A2XX_RB_DEPTHCONTROL_STENCIL_ENABLE |
                             A2XX_RB_DEPTHCONTROL_Z_ENABLE |
                             A2XX_RB_DEPTHCONTROL_ZFUNC(FUNC_ALWAYS);
  const UInt32 refAndReadMask = A2XX_RB_STENCILREFMASK_STENCILREF(1) |
                                A2XX_RB_STENCILREFMASK_STENCILMASK(0xff);
  writeRingRegister(REG_A2XX_RB_COLOR_MASK, 0);
  writeRingRegister(REG_A2XX_RB_STENCILREFMASK, refAndReadMask |
                                           A2XX_RB_STENCILREFMASK_STENCILWRITEMASK(0xff));
  writeRingRegister(REG_A2XX_RB_DEPTHCONTROL, depthAlways |
                                          A2XX_RB_DEPTHCONTROL_STENCILFUNC(FUNC_ALWAYS) |
                                          A2XX_RB_DEPTHCONTROL_STENCILZPASS(STENCIL_REPLACE));
  for (UInt32 index = 0; index < kXenosQuadStencilRects; ++index) {
    const UInt32 position = vertices + index * kQuadGeometryStride;
    drawTextureLayer(position, position + CACHELINE_SIZE, texture);
  }

  // The translucent quad reads stencil 1. KEEP is zero for all outcomes.
  writeRingRegister(REG_A2XX_RB_COLOR_MASK, colorMask);
  writeRingRegister(REG_A2XX_RB_STENCILREFMASK, refAndReadMask);
  writeRingRegister(REG_A2XX_RB_DEPTHCONTROL, depthAlways |
                                          A2XX_RB_DEPTHCONTROL_STENCILFUNC(FUNC_EQUAL));
}


int main() {
  XenosController test;
  test.stampQuadStencil(0x4000, 0x8000, 0xf);
  const std::vector<UInt32> expected = {
    0x2104, 0, 0x210d, 0xffff01, 0x2200, 0x8773,
    0x4000, 0x4100,
    0x2104, 0xf, 0x210d, 0xff01, 0x2200, 0x273,
  };
  assert(test.words == expected);
  test.words.clear();
  test.writeRingRegister(0x231d, 0xffffff00);
  test.writeRingRegister(0x2203, 0);
  assert((test.words == std::vector<UInt32>{0x231d, 0xffffff00, 0x2203, 0}));
  puts("Verified the emitted stencil, depth and color register packet indices and values.");
}
