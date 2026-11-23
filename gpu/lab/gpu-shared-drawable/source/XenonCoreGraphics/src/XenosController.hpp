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
  UInt32                     _surfaceSerial;
  UInt32                     _surfaceCount;
  UInt32                     _surfaceColor;
  IOBufferMemoryDescriptor   *_surfaceJobPixels;
  IOBufferMemoryDescriptor   *_drawableGrantPixels;
  XenosDrawableToken          _drawableGrantToken;
  UInt32                     _drawableReaders;
  struct QuadLayer {
    XenosQuadGeometryUpload payload;
    XenosQuadMapping mapping;
    XenosQuadRotation rotation;
    bool rotated;
  };
  QuadLayer                  _quadLayers[kXenosQuadBatchLayers];
  UInt8                      _quadFrontAlpha[kXenosQuadTexturePixels];
  XenosQuadScissor           _quadFrontMask[kXenosQuadStencilRects];
  XenosQuadScissor           _quadFrontScissor;
  UInt32                     _quadDepthSplitX;
  UInt32                     _quadLayerCount;
  UInt32                     _quadGeometryMode;
  UInt32                     _quadFirstOnly;
  UInt32                     _quadSecondOnly;
  UInt32                     _quadOverlap;
  UInt32                     _quadTranslucentOverlap;
  UInt32                     _quadMaxBlendDelta;
  UInt32                     _quadClippedBack;
  UInt32                     _quadClippedClear;
  UInt32                     _quadScissorRejectOnly;
  UInt32                     _quadStencilRejectOnly;
  UInt32                     _quadBothReject;
  UInt32                     _quadDepthReject;
  UInt32                     _quadSampleChecked;
  UInt32                     _quadSampleMismatches;
  UInt32                     _quadSampleTexels;
  UInt32                     _quadSampleGuard;
  UInt32                     _quadSampleHash;
  UInt32                     _quadSampleMaxDelta;
  UInt32                     _quadBilinearWitnesses;
  UInt32                     _quadMipChecked;
  UInt32                     _quadMipMismatches;
  UInt32                     _quadMipHash;
  UInt32                     _quadMipBaseMatches;
  UInt32                     _quadMipLevel1Matches;
  UInt32                     _quadMipLevel1Evidence;
  UInt32                     _quadTrilinearMatches;
  UInt32                     _quadTrilinearWitnesses;
  UInt32                     _quadTrilinearMaxDelta;
  UInt32                     _quadAnisoControl;
  UInt32                     _quadAnisoDark;
  UInt32                     _quadAnisoLight;
  UInt32                     _quadAnisoDarkMax;
  UInt32                     _quadAnisoLightMin;
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
  inline void writeRingRegister(UInt16 index, UInt32 data) {
    writeRingPacket0(index, 1);
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
  void setGpuWindowScissor(UInt32 left, UInt32 top, UInt32 right, UInt32 bottom,
                            bool disableOffset = false);
  void setGpuTexture(UInt32 address, UInt32 width, UInt32 height,
                     enum sq_tex_filter filter, bool tiled, UInt32 mipAddress = 0,
                     enum sq_tex_filter mipFilter = SQ_TEX_FILTER_POINT, UInt32 lodBiasSteps = 0,
                     UInt32 anisoFilter = 0);
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
  static IOReturn queueQuadBlendedBatch(OSObject *owner, void *client, void *input, void *output, void *);
  static IOReturn queueQuadScissoredBatch(OSObject *owner, void *client, void *input, void *output, void *);
  static IOReturn queueQuadStencilBatch(OSObject *owner, void *client, void *input, void *output, void *);
  static IOReturn queueQuadScissorStencilBatch(OSObject *owner, void *client, void *input, void *output, void *);
  static IOReturn queueQuadDepthBatch(OSObject *owner, void *client, void *input, void *output, void *);
  static IOReturn queueQuadRenderTextureBatch(OSObject *owner, void *client, void *input, void *output, void *);
  static IOReturn queueQuadBilinearBatch(OSObject *owner, void *client, void *input, void *output, void *);
  static IOReturn queueQuadMipmapBatch(OSObject *owner, void *client, void *input, void *output, void *);
  static IOReturn queueQuadTrilinearBatch(OSObject *owner, void *client, void *input, void *output, void *);
  static IOReturn queueQuadAnisotropicBatch(OSObject *owner, void *client, void *input, void *output, void *);
  static IOReturn createClientSurface(OSObject *owner, void *client, void *width, void *height, void *handle);
  static IOReturn queueSurfaceClear(OSObject *owner, void *client, void *handle, void *argb, void *ticket);
  static IOReturn readClientSurface(OSObject *owner, void *client, void *input, void *output, void *);
  static IOReturn releaseClientSurface(OSObject *owner, void *client, void *handle, void *, void *);
  static IOReturn closeClientSurface(OSObject *owner, void *client, void *, void *, void *);
  void dropSurface(XenosQuadClient *client);
  bool ownsSurface(XenosQuadClient *client, UInt32 handle) const;
  bool submitSurfaceClear();
  void verifySurfaceClear(UInt32 fence);
  static IOReturn grantDrawable(OSObject *owner, void *client, void *handle, void *token, void *);
  static IOReturn openDrawable(OSObject *owner, void *client, void *token, void *, void *);
  static IOReturn getDrawableView(OSObject *owner, void *client, void *options, void *descriptor, void *);
  void revokeDrawableGrant();
  void restoreQuadDisplayAndFence(UInt32 fence);
  IOReturn enqueueQuadDepthBatch(XenosQuadClient *client, const XenosQuadDepthBatchUpload &upload,
                                 UInt32 mode, UInt32 *output);
  IOReturn enqueueQuadBatch(XenosQuadClient *client, const XenosQuadBatchUpload &batch,
                            const UInt8 *frontAlpha, UInt32 mode, const XenosQuadScissor *frontMask,
                            const XenosQuadScissor *frontScissor, UInt32 *output,
                            UInt32 depthSplitX = 0);
  IOReturn quadQueueReady(XenosQuadClient *client);
  bool prepareQuadLayer(QuadLayer &layer, const XenosQuadGeometry &geometry,
                        const UInt8 *rgb, bool rotated);
  void startQuad(XenosQuadClient *client, UInt32 texture, bool uploaded,
                 UInt32 geometryMode, UInt32 layers, UInt32 *output);
  IOReturn enqueueQuad(XenosQuadClient *client, const UInt8 *rgb, UInt32 texture,
                       const XenosQuadGeometry *geometry, UInt32 geometryMode, UInt32 *output);
  static IOReturn readQuadStatus(OSObject *owner, void *client, void *ticket, void *output, void *);
  bool quadSampleAt(const QuadLayer &layer, UInt32 x, UInt32 y, UInt32 &u, UInt32 &v) const;
  bool quadFrontPassesStencil(UInt32 x, UInt32 y) const;
  bool quadUsesScissor() const;
  bool quadUsesStencil() const;
  bool quadUsesDepth() const;
  bool quadUsesRenderTexture() const;
  bool quadUsesBlending() const;
  UInt32 quadLayerTexel(UInt32 layer, UInt32 u, UInt32 v) const;
  bool stepQuad();
  bool submitQuad();
  void finishQuad(UInt32 state, UInt32 fence, UInt32 checked, UInt32 mismatches, UInt32 hash = 0);
  void drawTextureLayer(UInt32 positions, UInt32 uv, UInt32 texture,
                         enum sq_tex_filter filter = SQ_TEX_FILTER_POINT, UInt32 mipAddress = 0,
                        enum sq_tex_filter mipFilter = SQ_TEX_FILTER_POINT, UInt32 lodBiasSteps = 0,
                     UInt32 anisoFilter = 0);
  void stampQuadStencil(UInt32 vertices, UInt32 texture, UInt32 colorMask,
                         enum adreno_compare_func frontDepthFunc);
  void setQuadBlendState(bool enabled);
  void resolveQuadColor(UInt32 address);
  void flushQuadResolve(UInt32 address);
  void clearQuadSampleTarget(UInt32 discard, UInt32 clearColor);
  void beginQuadSampleDraw();
  void drawQuadTexturePass(UInt32 texture, UInt32 positions, UInt32 discard, UInt32 result,
                           enum sq_tex_filter filter);
  void verifyQuadTexturePass(const volatile UInt32 *first, volatile UInt32 *sample);
  void verifyQuadMipmapPass(const volatile UInt32 *base, volatile UInt32 *mip, volatile UInt32 *sample);
  void verifyQuadTrilinearSamples(const volatile UInt32 *base, const volatile UInt32 *mip,
                                  const volatile UInt32 *sample);
  void verifyQuadAnisotropicPass(volatile UInt32 *sample);
  void syncGpuMemory(UInt32 address, UInt32 bytes, UInt32 actions);
  void syncData(volatile void *data, UInt32 length);
  void syncRingBuffer(void);
};

#endif
