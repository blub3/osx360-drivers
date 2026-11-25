//
//  XenosGPU.hpp
//  Xenos GPU driver for Mac OS X 10.4 on the Xbox 360 (GPU plan, docs/gpu-driver-plan.md).
//
//  Step 1.1: loaded by hand (tools/host/gpu-dev.sh), never at boot. It takes the GPU from the
//  platform's CPU framebuffer refresh, starts the command processor with the checked reset sequence
//  from XenonCoreGraphics 0.0.5d46, presents the desktop with the GPU every 15 ms, and gives the
//  display back to the platform on unload. Ring, firmware and display code follow XenonCoreGraphics
//  (Copyright (c) 2026 John Davis and contributors), which ran on hardware.
//

#ifndef XenosGPU_hpp
#define XenosGPU_hpp

#include <IOKit/IOService.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>

#include "XenonCommon.hpp"
#include "XenosRegs.hpp"

static inline UInt32 XenosFloatBits(float f) {
  union {
    float f;
    UInt32 u;
  } value;
  value.f = f;
  return value.u;
}
#define fui XenosFloatBits

#include "../../XenonCoreGraphics/src/adreno_pm4.xml.h"
#include "../../XenonCoreGraphics/src/adreno_common.xml.h"
#include "../../XenonCoreGraphics/src/a2xx.xml.h"

#define XENOS_CP_REG(reg) ((0x4 << 16) | ((unsigned int)((reg) - (0x2000))))

class XenosGPU : public IOService {
  OSDeclareDefaultStructors(XenosGPU);
  typedef IOService super;

public:
  bool start(IOService *provider);
  void stop(IOService *provider);
  void free(void);

private:
  // MMIO and display.
  IOMemoryMap               *_mmioMap;
  volatile void             *_mmio;
  IOPhysicalAddress         _scanoutPhys;
  IOPhysicalAddress         _linearPhys;
  UInt32                    _width;
  UInt32                    _height;

  // Command ring and helpers.
  IOBufferMemoryDescriptor  *_ringDesc;
  volatile UInt8            *_ring;
  IOPhysicalAddress         _ringPhys;
  UInt32                    _ringWrite;
  IOBufferMemoryDescriptor  *_markerDesc;
  volatile UInt32           *_marker;
  IOPhysicalAddress         _markerPhys;
  IOMemoryMap               *_heapMap;
  bool                      _useHeap;
  bool                      _libxenonInit;
  IOBufferMemoryDescriptor  *_texCoordsDesc;
  IOPhysicalAddress         _texCoordsPhys;
  IOBufferMemoryDescriptor  *_quadDesc;
  IOPhysicalAddress         _quadPhys;

  // Presentation.
  IOWorkLoop                *_workLoop;
  IOTimerEventSource        *_timer;
  bool                      _presenting;
  bool                      _platformStopped;
  UInt32                    _frames;
  UInt32                    _presentMS;
  UInt32                    _busyTicks;
  UInt32                    _busyRun;
  bool                      _stallLogged;

  inline void writeReg(UInt32 offset, UInt32 value) { OSWriteBigInt32(_mmio, offset, value); }
  inline UInt32 readReg(UInt32 offset) { return OSReadBigInt32(_mmio, offset); }
  inline void ring(UInt32 value) {
    *(volatile UInt32 *)(_ring + _ringWrite * 4) = value;
    if (++_ringWrite == kXenosRingbufferLength) {
      _ringWrite = 0;
    }
  }
  inline void ringPacket0(UInt16 index, UInt16 count) {
    ring(CP_TYPE0_PKT | ((count - 1) << 16) | (index & 0x7FFF));
  }
  inline void ringPacket3(UInt8 opcode, UInt16 count) {
    ring(CP_TYPE3_PKT | ((count - 1) << 16) | ((opcode & 0xFF) << 8));
  }
  void ringConstant(UInt32 reg, UInt32 value);

  // XenosGPU_Ring.cpp
  IOReturn allocateBuffers(void);
  IOReturn allocateHeapBuffers(void);
  void releaseBuffers(void);
  IOReturn loadFirmware(void);
  void haltCommandProcessor(void);
  void libxenonCPRegisters(void);
  void libxenonMasterInit(void);
  bool waitEdramIdle(UInt32 *value);
  bool resetGraphicsBlocks(const char *when);
  bool ringDrained(void);
  bool waitForRingDrain(void);
  void submit(void);
  bool runMarker(const char *phase);
  void initMicroEngine(void);
  void initDisplayState(void);
  const char *startCommandProcessor(void);
  IOReturn startGPU(void);

  // XenosGPU.cpp
  void present(IOTimerEventSource *sender);
  void emitPresent(void);
  IOReturn callPlatform(const char *function);
};

#endif
