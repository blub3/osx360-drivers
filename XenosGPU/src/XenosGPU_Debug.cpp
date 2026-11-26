//
//  XenosGPU_Debug.cpp
//  Debug access, fences and the GPU interrupt (GPU plan step 1.2).
//
//  Register reads: any 32-bit register in the 64 KiB MMIO window except data ports whose reads have
//  side effects (PFP and ME firmware data, CP state debug data). Register writes: only the CP and
//  RBBM interrupt control and acknowledge registers. Fences: a CP_MEM_WRITE of an increasing
//  sequence into the fence page (GPU heap + 0xB000), submitted under the work loop's command gate so
//  it never interleaves with a frame. Waiting polls the fence page (1 ms sleeps).
//
//  The GPU interrupt (vector 0x58, OpenBIOS gives xenos@2 "interrupts" = 0x58) is registered only on
//  request (kXenosGPUDebugInterrupt) and removed again on request or at stop. Registered but disabled
//  would be worse than unregistered: XenonPE silences an unclaimed 0x58, a registered one it leaves
//  to the driver.
//

#include "XenosGPU.hpp"
#include <IOKit/IOFilterInterruptEventSource.h>

static const UInt32 kRegCpIntCntl     = 0x1F2 * 4;
static const UInt32 kRegCpIntStatus   = 0x1F3 * 4;
static const UInt32 kRegCpIntAck      = 0x1F4 * 4;
static const UInt32 kRegRbbmIntCntl   = 0x3B4 * 4;
static const UInt32 kRegRbbmIntStatus = 0x3B5 * 4;
static const UInt32 kRegRbbmIntAck    = 0x3B6 * 4;
static const UInt32 kFenceSwap8In32   = 2;

IOReturn XenosGPU::debugRead(UInt32 offset, UInt32 *value) {
  if (offset >= 0x10000 || (offset & 3) != 0) {
    return kIOReturnBadArgument;
  }
  if (offset == kXenosRegCpPfpFirmwareData || offset == kXenosRegCpMeRamData || offset == 0x1ED * 4) {
    return kIOReturnNotPermitted;
  }
  *value = readReg(offset);
  return kIOReturnSuccess;
}

IOReturn XenosGPU::debugWrite(UInt32 offset, UInt32 value) {
  if (offset != kRegCpIntCntl && offset != kRegCpIntAck && offset != kRegRbbmIntCntl &&
      offset != kRegRbbmIntAck) {
    return kIOReturnNotPermitted;
  }
  writeReg(offset, value);
  IOLog("XenosGPU: debug write 0x%04lx = 0x%08lx\n", (unsigned long)offset, (unsigned long)value);
  return kIOReturnSuccess;
}

IOMemoryDescriptor *XenosGPU::fenceMemory(void) {
  return _fenceDesc;
}

IOReturn XenosGPU::fenceAction(OSObject *owner, void *sequence, void *, void *, void *) {
  XenosGPU *self = (XenosGPU *)owner;
  if (!self->_presenting || self->_fence == NULL) {
    return kIOReturnNotReady;
  }
  UInt32 seq = ++self->_fenceSubmitted;
  self->ringPacket3(CP_MEM_WRITE, 2);
  self->ring((UInt32)self->_fencePhys | kFenceSwap8In32);
  self->ring(seq);
  self->submit();
  *(UInt32 *)sequence = seq;
  return kIOReturnSuccess;
}

IOReturn XenosGPU::submitFence(UInt32 *sequence) {
  if (_gate == NULL) {
    return kIOReturnNotReady;
  }
  return _gate->runAction(&XenosGPU::fenceAction, sequence);
}

IOReturn XenosGPU::waitFence(UInt32 sequence, UInt32 timeoutMS, UInt32 *elapsedUS, UInt32 *completed) {
  if (_fence == NULL) {
    return kIOReturnNotReady;
  }
  // Time base (50 MHz) rather than clock_get_uptime, to keep the kernel imports of the main6 boot set.
  UInt32 start, now;
  __asm__ volatile ("mftb %0" : "=r" (start));
  if (timeoutMS > 10000) {
    timeoutMS = 10000;
  }
  for (UInt32 slept = 0; ; slept++) {
    *completed = _fence[kXenosGPUFenceWordSequence];
    if ((SInt32)(*completed - sequence) >= 0 || slept >= timeoutMS) {
      break;
    }
    IOSleep(1);
  }
  __asm__ volatile ("mftb %0" : "=r" (now));
  *elapsedUS = (now - start) / 50;
  return (SInt32)(*completed - sequence) >= 0 ? kIOReturnSuccess : kIOReturnTimeout;
}

void XenosGPU::debugStatistics(UInt32 *frames, UInt32 *busyTicks, UInt32 *fencesSubmitted,
                               UInt32 *fenceCompleted, UInt32 *interrupts, UInt32 *lastStatus) {
  *frames = _frames;
  *busyTicks = _busyTicks;
  *fencesSubmitted = _fenceSubmitted;
  *fenceCompleted = (_fence != NULL) ? _fence[kXenosGPUFenceWordSequence] : 0;
  *interrupts = _interrupts;
  *lastStatus = _lastInterruptStatus;
}

//
// Primary interrupt context: acknowledge what the CP and RBBM report, count, never schedule work.
//
bool XenosGPU::interruptFilter(OSObject *owner, IOFilterInterruptEventSource *source) {
  XenosGPU *self = (XenosGPU *)owner;
  UInt32 cp = self->readReg(kRegCpIntStatus);
  UInt32 rbbm = self->readReg(kRegRbbmIntStatus);
  if (cp != 0) {
    self->writeReg(kRegCpIntAck, cp);
  }
  if (rbbm != 0) {
    self->writeReg(kRegRbbmIntAck, rbbm);
  }
  self->_interrupts++;
  self->_lastInterruptStatus = cp;
  self->_lastInterruptRbbm = rbbm;
  return false;
}

void XenosGPU::interruptHandler(OSObject *, IOInterruptEventSource *, int) {
}

IOReturn XenosGPU::setInterrupt(bool enable) {
  if (_workLoop == NULL) {
    return kIOReturnNotReady;
  }
  if (enable && _interruptSource == NULL) {
    _interruptSource = IOFilterInterruptEventSource::filterInterruptEventSource(this,
        &XenosGPU::interruptHandler, &XenosGPU::interruptFilter, getProvider(), 0);
    if (_interruptSource == NULL || _workLoop->addEventSource(_interruptSource) != kIOReturnSuccess) {
      IOLog("XenosGPU: cannot register the GPU interrupt\n");
      if (_interruptSource != NULL) {
        _interruptSource->release();
        _interruptSource = NULL;
      }
      return kIOReturnNoResources;
    }
    _interruptSource->enable();
    IOLog("XenosGPU: GPU interrupt registered and enabled (CP_INT_CNTL=0x%08lx RBBM_INT_CNTL=0x%08lx)\n",
          (unsigned long)readReg(kRegCpIntCntl), (unsigned long)readReg(kRegRbbmIntCntl));
  } else if (!enable && _interruptSource != NULL) {
    _interruptSource->disable();
    _workLoop->removeEventSource(_interruptSource);
    _interruptSource->release();
    _interruptSource = NULL;
    IOLog("XenosGPU: GPU interrupt removed after %lu interrupts (last CP 0x%08lx RBBM 0x%08lx)\n",
          (unsigned long)_interrupts, (unsigned long)_lastInterruptStatus,
          (unsigned long)_lastInterruptRbbm);
  }
  return kIOReturnSuccess;
}
