//
//  XenosFB.hpp
//  Xbox 360 graphics controller
//
//  Copyright © 2026 John Davis. All rights reserved.
//

#ifndef XenosController_hpp
#define XenosController_hpp

#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOService.h>

#include "XenonCommon.hpp"
#include "XenosRegs.hpp"
#include "XenosQuadProtocol.h"

static inline UInt32 fui(float f) {
	union {
		float f;
		UInt32 ui;
	} fi;
	fi.f = f;
	return fi.ui;
}

#include "adreno_pm4.xml.h"
#include "adreno_common.xml.h"
#include "a2xx.xml.h"

static const UInt32 kResolveConvert = 1 << 20;
static const UInt32 kResolveDisplayFormat = (COLORX_S8_8_8_8 << 7) | (1 << 24);

#define RADEON_CP_PACKET0               0x00000000
#define RADEON_ONE_REG_WR                (1 << 15)

#define CP_REG(reg) ((0x4 << 16) | ((unsigned int)((reg) - (0x2000))))
#define CP_PACKET0( reg, n )                                            \
        (RADEON_CP_PACKET0 | ((n) << 16) | ((reg) >> 2))
#define CP_PACKET0_TABLE( reg, n )                                      \
        (RADEON_CP_PACKET0 | RADEON_ONE_REG_WR | ((n) << 16) | ((reg) >> 2))

//
// Represents the Xbox 360 graphics controller.
// Functionality implemented here to provide core support even if IOGraphicsFamily is not present.
//
class XenosQuadClient;

class XenosController : public IOService {
  friend class XenosQuadClient;
  OSDeclareDefaultStructors(XenosController);
  XenonDeclareLogFunctions("xenosctrl");
  typedef IOService super;

public:
  // IOService overrides.
  bool start(IOService *provider);

private:
  // MMIO and framebuffer.
  IOMemoryMap         *_mmioMap;
  volatile void       *_mmioMem;

  IOWorkLoop          *_timerWorkLoop;
  IOTimerEventSource  *_timerEventSource;

  IOPhysicalAddress   _fbPhysAddr;
  IOPhysicalAddress   _gpuPhysAddr;

  // Current display info.
  UInt32              _fbWidth;
  UInt32              _fbHeight;

  // Ring buffer.
  IOBufferMemoryDescriptor  *_ringBufferDesc;
  volatile UInt8            *_ringBuffer;
  UInt32                    _ringBufferWritePtr;
  UInt32                    _refreshesUntilRingSample;
  UInt32                    _markerSubmitAttempts;
  IOBufferMemoryDescriptor  *_markerScratch;
  IOCommandGate             *_quadGate;
  XenosQuadClient            *_quadClient;
  IOBufferMemoryDescriptor   *_quadScratch;
  UInt32                     _quadTicket;
  struct QuadLayer {
    XenosQuadGeometryUpload payload;
    XenosQuadMapping mapping;
    XenosQuadRotation rotation;
    bool rotated;
  };
  QuadLayer                  _quadLayers[kXenosQuadBatchLayers];
  UInt32                     _quadLayerCount;
  UInt32                     _quadGeometryMode;
  UInt32                     _quadFirstOnly;
  UInt32                     _quadSecondOnly;
  UInt32                     _quadOverlap;
  bool                       _quadUploaded;
  UInt32                     _quadTexture;
  UInt32                     _quadWait;
  UInt32                     _quadCooldown;

  // Shaders.
  IOBufferMemoryDescriptor  *_texCoordsDesc;
  IOPhysicalAddress         _texCoordsPhys;
  IOBufferMemoryDescriptor  *_triagDesc;
  IOPhysicalAddress         _triagPhys;

  // Register read/write.
  inline void writeReg32(UInt32 offset, UInt32 data) {
    OSWriteBigInt32(_mmioMem, offset, data);
  }
  inline UInt32 readReg32(UInt32 offset) {
    return OSReadBigInt32(_mmioMem, offset);
  }

  // Ring buffer writes.
  inline void writeRing(UInt32 data) {
    *(volatile UInt32*)(_ringBuffer + (_ringBufferWritePtr++ * 4)) = data;
    if (_ringBufferWritePtr == kXenosRingbufferLength) {
      _ringBufferWritePtr = 0;
    }
  }
  inline void writeRingReg32(UInt32 reg, UInt32 data) {
    writeRing(CP_PACKET0(reg, 0));
    writeRing(data);
  }
  inline void writeRingPacket0(UInt16 index, UInt16 length) {
    writeRing(CP_TYPE0_PKT | ((length - 1) << 16) | (index & 0x7FFF));
  }
  inline void writeRingPacket3(UInt8 opcode, UInt16 length) {
    writeRing(CP_TYPE3_PKT | ((length - 1) << 16) | ((opcode & 0xFF) << 8));
  }

  // Firmware.
  IOReturn loadFirmware(void);

  // Setup.
  IOReturn initShaders(void);
  void initME(void);
  IOReturn initHardware(void);
  void initFramebuffer(void);
  void setGpuViewport(UInt32 width, UInt32 height);
  void setGpuScissors(UInt32 width, UInt32 height);
  void setGpuTexture(UInt32 address, UInt32 width, UInt32 height,
                     enum sq_tex_filter filter, bool tiled);
  void setGpuDisplayVertices();
  void setGpuVertices(UInt32 positionAddress, UInt32 positionBytes,
                      UInt32 uvAddress, UInt32 uvBytes);
  void setGpuDisplayShaders();
  void setGpuTexturePixelShader(bool explicit2D);
  void setGpuEdRamMode(enum a2xx_rb_edram_mode mode);
  void setResolveTarget(UInt32 address, UInt32 width, UInt32 height,
                        UInt32 control, UInt32 format);

  // Internal functions.
  void handleTimer(IOTimerEventSource *sender);
  void runMarkerTest();
  void drawResolveRectangle();
  static IOReturn queueQuad(OSObject *owner, void *client, void *rgb, void *output, void *texture);
  static IOReturn queueQuadGeometry(OSObject *owner, void *client, void *input, void *output, void *);
  static IOReturn queueQuadRotated(OSObject *owner, void *client, void *input, void *output, void *);
  static IOReturn queueQuadBatch(OSObject *owner, void *client, void *input, void *output, void *);
  IOReturn quadQueueReady(XenosQuadClient *client);
  bool prepareQuadLayer(QuadLayer &layer, const XenosQuadGeometry &geometry,
                        const UInt8 *rgb, bool rotated);
  void startQuad(XenosQuadClient *client, UInt32 texture, bool uploaded,
                 UInt32 geometryMode, UInt32 layers, UInt32 *output);
  IOReturn enqueueQuad(XenosQuadClient *client, const UInt8 *rgb, UInt32 texture,
                       const XenosQuadGeometry *geometry, UInt32 geometryMode, UInt32 *output);
  static IOReturn readQuadStatus(OSObject *owner, void *client, void *ticket, void *output, void *);
  bool quadSampleAt(const QuadLayer &layer, UInt32 x, UInt32 y, UInt32 &u, UInt32 &v) const;
  bool stepQuad();
  bool submitQuad();
  void finishQuad(UInt32 state, UInt32 fence, UInt32 checked, UInt32 mismatches, UInt32 hash = 0);
  void drawTextureLayer(UInt32 positions, UInt32 uv, UInt32 texture);
  void resolveQuadColor(UInt32 address);
  void syncGpuMemory(UInt32 address, UInt32 bytes, UInt32 actions);
  void syncData(volatile void *data, UInt32 length);
  void syncRingBuffer(void);
};

#endif
