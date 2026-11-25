//
//  XenosGPU.cpp
//  Xenos GPU driver, start/stop and presentation (GPU plan step 1.1).
//

#include "XenosGPU.hpp"
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceMemory.h>

OSDefineMetaClassAndStructors(XenosGPU, IOService);

static const UInt32 kPresentIntervalMS = 15;
static const UInt32 kXenosMmioBase = 0xEC800000;
static const UInt32 kXenosMmioSize = 0x10000;

static inline UInt32 drawInitiator(pc_di_primtype prim, pc_di_src_sel source, pc_di_index_size size,
                                   pc_di_vis_cull_mode cull) {
  return (prim << 0) | (source << 6) | ((size & 1) << 11) | ((size >> 1) << 13) | (cull << 9) | (1 << 14);
}

IOReturn XenosGPU::callPlatform(const char *function) {
  const OSSymbol *symbol = OSSymbol::withCString(function);
  if (symbol == NULL) {
    return kIOReturnNoMemory;
  }
  IOReturn result = getPlatform()->callPlatformFunction(symbol, false, NULL, NULL, NULL, NULL);
  symbol->release();
  return result;
}

bool XenosGPU::start(IOService *provider) {
  _mmioMap = NULL;
  _ringDesc = _markerDesc = _texCoordsDesc = _quadDesc = NULL;
  _ring = NULL;
  _marker = NULL;
  _heapMap = NULL;
  _useHeap = false;
  _workLoop = NULL;
  _timer = NULL;
  _presenting = _platformStopped = false;
  _frames = _busyTicks = _busyRun = 0;
  _stallLogged = false;

  if (!super::start(provider)) {
    return false;
  }
  _presentMS = (getProperty("XenosGPUSlowPresent") == kOSBooleanTrue) ? 50 : kPresentIntervalMS;
  IOLog("XenosGPU: start (step 1.1: GPU presentation every %lu ms)\n", (unsigned long)_presentMS);

  IODeviceMemory *mmio = IODeviceMemory::withRange(kXenosMmioBase, kXenosMmioSize);
  if (mmio != NULL) {
    _mmioMap = mmio->map();
    mmio->release();
  }
  if (_mmioMap == NULL) {
    IOLog("XenosGPU: cannot map registers\n");
    return false;
  }
  _mmio = (volatile void *)_mmioMap->getVirtualAddress();

  // The platform expert points the display at the second 8 MiB buffer and keeps Tiger's linear
  // framebuffer in the first (XenonPE::mapGPU).
  _scanoutPhys = readReg(kXenosRegD1GrphPriSurfaceAddr);
  _linearPhys = _scanoutPhys - kXenosFramebufferLength;
  PE_Video video;
  getPlatform()->getConsoleInfo(&video);
  _width = video.v_width;
  _height = video.v_height;
  IOLog("XenosGPU: display %lux%lu, linear 0x%08lx, scanout 0x%08lx\n", (unsigned long)_width,
        (unsigned long)_height, (unsigned long)_linearPhys, (unsigned long)_scanoutPhys);
  if (_width == 0 || _height == 0 || _width > 2048 || _height > 2048 ||
      _scanoutPhys != 0x1F800000) {
    IOLog("XenosGPU: unexpected display layout, not taking the GPU\n");
    return false;
  }

  if (startGPU() != kIOReturnSuccess) {
    IOLog("XenosGPU: GPU start failed, the CPU refresh keeps the display\n");
    releaseBuffers();
    return false;
  }

  _workLoop = IOWorkLoop::workLoop();
  _timer = IOTimerEventSource::timerEventSource(this,
      OSMemberFunctionCast(IOTimerEventSource::Action, this, &XenosGPU::present));
  if (_workLoop == NULL || _timer == NULL || _workLoop->addEventSource(_timer) != kIOReturnSuccess) {
    IOLog("XenosGPU: no timer\n");
    haltCommandProcessor();
    return false;
  }

  // Take the display from the platform's CPU refresh, then present with the GPU.
  IOReturn status = callPlatform(kXenonFuncPlatformStopFB);
  _platformStopped = (status == kIOReturnSuccess);
  IOLog("XenosGPU: CPU framebuffer refresh stopped: 0x%08lx\n", (unsigned long)status);
  IOLog("XenosGPU: interrupts at start: CP_INT_CNTL=0x%08lx CP_INT_STATUS=0x%08lx RBBM_INT_CNTL=0x%08lx "
        "RBBM_INT_STATUS=0x%08lx MASTER_INT_SIGNAL=0x%08lx\n", (unsigned long)readReg(0x1F2 * 4),
        (unsigned long)readReg(0x1F3 * 4), (unsigned long)readReg(0x3B4 * 4),
        (unsigned long)readReg(0x3B5 * 4), (unsigned long)readReg(0x3B7 * 4));
  _presenting = true;
  _timer->enable();
  _timer->setTimeoutMS(_presentMS);
  registerService();
  return true;
}

void XenosGPU::stop(IOService *provider) {
  IOLog("XenosGPU: stop after %lu frames (%lu ticks waited for a busy ring)\n",
        (unsigned long)_frames, (unsigned long)_busyTicks);
  _presenting = false;
  if (_timer != NULL) {
    _timer->cancelTimeout();
    _timer->disable();
    if (_workLoop != NULL) {
      _workLoop->removeEventSource(_timer);
    }
  }
  if (_mmio != NULL && _ring != NULL) {
    bool drained = waitForRingDrain();
    haltCommandProcessor();
    IOLog("XenosGPU: ring %s, command processor halted\n", drained ? "drained" : "NOT drained");
  }
  if (_platformStopped) {
    IOReturn status = callPlatform(kXenonFuncPlatformStartFB);
    IOLog("XenosGPU: CPU framebuffer refresh started again: 0x%08lx\n", (unsigned long)status);
    _platformStopped = false;
  }
  super::stop(provider);
}

void XenosGPU::free(void) {
  if (_timer != NULL) {
    _timer->release();
    _timer = NULL;
  }
  if (_workLoop != NULL) {
    _workLoop->release();
    _workLoop = NULL;
  }
  releaseBuffers();
  if (_mmioMap != NULL) {
    _mmioMap->release();
    _mmioMap = NULL;
  }
  super::free();
}

//
// One frame: draw the linear framebuffer into EDRAM, then resolve it into the tiled scanout buffer.
//
void XenosGPU::emitPresent(void) {
  ringConstant(REG_A2XX_RB_MODECONTROL, A2XX_RB_MODECONTROL_EDRAM_MODE(COLOR_DEPTH));
  ringPacket3(CP_DRAW_INDX, 2);
  ring(0x00000000);
  ring((4 << 16) | drawInitiator(DI_PT_TRISTRIP, DI_SRC_SEL_AUTO_INDEX, INDEX_SIZE_IGN, IGNORE_VISIBILITY));
  ringConstant(REG_A2XX_RB_MODECONTROL, A2XX_RB_MODECONTROL_EDRAM_MODE(EDRAM_COPY));
  ringPacket3(CP_DRAW_INDX, 2);
  ring(0x00000000);
  ring((3 << 16) | drawInitiator(DI_PT_RECTLIST, DI_SRC_SEL_AUTO_INDEX, INDEX_SIZE_IGN, IGNORE_VISIBILITY));
  submit();
}

void XenosGPU::present(IOTimerEventSource *sender) {
  if (!_presenting) {
    return;
  }
  if (ringDrained()) {
    emitPresent();
    _frames++;
    _stallLogged = false;
  } else {
    _busyTicks++;
    // Log the first stall of each run: ring pointers and status after 10 busy ticks (150 ms).
    if (++_busyRun == 10 && !_stallLogged) {
      _stallLogged = true;
      IOLog("XenosGPU: stall after %lu frames: rptr=0x%04lx wptr=0x%04lx sw=0x%04lx RBBM_STATUS=0x%08lx "
            "CP_ME_STATUS=0x%08lx CP_STAT=0x%08lx\n", (unsigned long)_frames,
            (unsigned long)readReg(kXenosRegCpRbReadPtr), (unsigned long)readReg(kXenosRegCpRbWritePtr),
            (unsigned long)_ringWrite, (unsigned long)readReg(kXenosRegRbbmStatus),
            (unsigned long)readReg(kXenosRegCpMeStatus), (unsigned long)readReg(0x47F * 4));
      // Interrupt registers: XenonPE silences unclaimed GPU vector 0x58 by writing CP/RBBM_INT_CNTL.
      IOLog("XenosGPU: stall interrupts: CP_INT_CNTL=0x%08lx CP_INT_STATUS=0x%08lx RBBM_INT_CNTL=0x%08lx "
            "RBBM_INT_STATUS=0x%08lx MASTER_INT_SIGNAL=0x%08lx\n", (unsigned long)readReg(0x1F2 * 4),
            (unsigned long)readReg(0x1F3 * 4), (unsigned long)readReg(0x3B4 * 4),
            (unsigned long)readReg(0x3B5 * 4), (unsigned long)readReg(0x3B7 * 4));
    }
    sender->setTimeoutMS(_presentMS);
    return;
  }
  _busyRun = 0;
  sender->setTimeoutMS(_presentMS);
}
