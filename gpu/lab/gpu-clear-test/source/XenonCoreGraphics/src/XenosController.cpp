//
//  XenosController.cpp
//  Xbox 360 graphics controller
//
//  Copyright © 2026 John Davis. All rights reserved.
//

#include <IOKit/IOPlatformExpert.h>

#include "XenosController.hpp"

OSDefineMetaClassAndStructors(XenosController, super);

#define CACHELINE_SIZE 128

// The refresh loop schedules its next callback every 15 ms.
static const UInt32 kRefreshesPerRingSample = 320;

static const UInt32 kMarkerSubmitAttempts = 16;
static const UInt32 kMarkerPacketDwords = 3;
static const UInt32 kMarkerPollLimit = 2000;
static const UInt32 kMarkerPollDelayUs = 10;
static const UInt32 kMarkerInitial = 0xa5a5a5a5;
static const UInt32 kMarkerExpected = 0x58454e4f;
// Xenos's address flag for swapping bytes within a 32-bit write.
static const UInt32 kMarkerSwap8In32 = 2;
// The two display buffers occupy the top 16 MiB of physical RAM.
static const IOPhysicalAddress kDisplayMemoryStart = 0x20000000 - 2 * kXenosFramebufferLength;

static const UInt32 kClearSubmitAttempts = 16;
static const UInt32 kClearRingDwords = 128; // Includes display state restore and the refresh draw.
static const UInt32 kClearSide = 32;
static const UInt32 kClearEdRamPitch = 160; // Two 80-pixel eDRAM tiles per row.
static const UInt32 kClearPixels = kClearSide * kClearSide;
static const UInt32 kClearBytes = kClearPixels * sizeof(UInt32);
static const UInt32 kClearTile = 0x7f0;
static const UInt32 kClearColor = 0xff37b24c;  // Packed eDRAM colour, red in the low byte.
static const UInt32 kClearExpected = 0xff4cb237; // Resolve swaps the red and blue bytes.
static const UInt32 kClearSentinel = 0xa5a5a5a5;
static const UInt32 kClearComplete = 0x434c4541;
static const UInt16 kXenosWaitUntil = 0x05c8;
static const UInt32 kWait3dIdleClean = 1 << 17;
static const UInt16 kXenosCoherDestBase1 = 0x2007;
static const UInt16 kXenosCoherSizeHost = 0x0a2f;
static const UInt16 kXenosCoherStatusHost = 0x0a31;
static const UInt32 kCoherResolveActions = 0x03000100;
static const UInt32 kCoherBusy = 1U << 31;
static const UInt16 kXenosColorClear = 0x231e;
static const UInt32 kResolveColorClear = 1 << 8;
static const UInt32 kCacheFlushEvent = 6;

//
// Performs driver startup.
// Overrides IOService::start().
//
bool XenosController::start(IOService *provider) {
  PE_Video videoInfo;
  IOReturn status;

  XenonCheckDebugArgs();

  if (!super::start(provider)) {
    XESYSLOG("super::start() returned false");
    return false;
  }

  XEDBGLOG("mmio %u", provider->getDeviceMemoryCount());

  // Map display adapter registers.
  _mmioMap = IODeviceMemory::withRange(0xEC800000, 0x10000)->map();
  //_mmioMap = provider->mapDeviceMemoryWithIndex(0);
  if (_mmioMap == NULL) {
    XESYSLOG("Failed to map display adapter registers");
    return false;
  }

  _mmioMem = (volatile void*) _mmioMap->getVirtualAddress();
  XEDBGLOG("Mapped display adapter registers at 0x%X length 0x%X to %p", _mmioMap->getPhysicalAddress(),
    _mmioMap->getLength(), _mmioMem);

  // Get current framebuffer.
  // This assumes the GPU has already been reconfigured by the platform expert.
  _gpuPhysAddr = readReg32(kXenosRegD1GrphPriSurfaceAddr);
  _fbPhysAddr = _gpuPhysAddr - kXenosFramebufferLength;
  XEDBGLOG("Current GPU framebuffer address: 0x%X", _fbPhysAddr);

  getPlatform()->getConsoleInfo(&videoInfo);
  _fbWidth = videoInfo.v_width;
  _fbHeight = videoInfo.v_height;

  // Initialize buffers.
  status = initShaders();
  if (status != kIOReturnSuccess) {
    XESYSLOG("Failed to allocate shaders (0x%X)", status);
    return false;
  }

  initHardware();
  XESYSLOG("hw init");
  IOSleep(1000);

  XESYSLOG("hardware init?");
  IOSleep(1000);
  initFramebuffer();

  XESYSLOG("fb init?");
  _timerWorkLoop = IOWorkLoop::workLoop();
  if (_timerWorkLoop == NULL) {
    XESYSLOG("Failed to initialize timer workloop");
    return false;
  }

  _timerEventSource = IOTimerEventSource::timerEventSource(this,
    OSMemberFunctionCast(IOTimerEventSource::Action, this, &XenosController::handleTimer));
  if (_timerEventSource == NULL) {
    XESYSLOG("Failed to initialize timer");
    return false;
  }

  _timerWorkLoop->addEventSource(_timerEventSource);
  _timerEventSource->enable();

  // Discontinue platform expert framebuffer services.
  const OSSymbol *funcSym = OSSymbol::withCStringNoCopy(kXenonFuncPlatformStopFB);
  status = getPlatform()->callPlatformFunction(funcSym, false, NULL, NULL, NULL, NULL);
  if (status != kIOReturnSuccess) {
    XESYSLOG("Failed to stop platform framebuffer");
    return false;
  }
  XEDBGLOG("Stopped platform framebuffer");

  // Start the refresh timer.
  _refreshesUntilRingSample = 1;
  _markerSubmitAttempts = kMarkerSubmitAttempts;
  _markerScratch = NULL;
  _clearSubmitAttempts = kClearSubmitAttempts;
  _clearScratch = NULL;
  _timerEventSource->setTimeoutMS(1000);
  return true;
}

static inline uint32_t DRAW(pc_di_primtype prim_type,
		pc_di_src_sel source_select, pc_di_index_size index_size,
		pc_di_vis_cull_mode vis_cull_mode)
{
	return (prim_type         << 0) |
			(source_select     << 6) |
			((index_size & 1)  << 11) |
			((index_size >> 1) << 13) |
			(vis_cull_mode     << 9) |
			(1                 << 14);
}

//
// Refresh timer handler.
//
void XenosController::handleTimer(IOTimerEventSource *sender) {
  if (--_refreshesUntilRingSample == 0) {
    // Sample before the next submission so the previous batch can drain.
    const UInt32 readPointer = readReg32(kXenosRegCpRbReadPtr);
    const UInt32 writePointer = readReg32(kXenosRegCpRbWritePtr);
    IOLog("XenosRing: rptr=0x%08lx wptr=0x%08lx sw_wptr=0x%08lx\n",
          readPointer, writePointer, _ringBufferWritePtr);
    _refreshesUntilRingSample = kRefreshesPerRingSample;
  }

  const bool markerWasPending = _markerSubmitAttempts != 0;
  if (markerWasPending) {
    // The work loop owns submissions. Wait for the previous batch to drain,
    // and keep this packet contiguous in the ring.
    if (_ringBufferWritePtr <= kXenosRingbufferLength - kMarkerPacketDwords &&
        readReg32(kXenosRegCpRbReadPtr) == _ringBufferWritePtr &&
        readReg32(kXenosRegCpRbWritePtr) == _ringBufferWritePtr) {
      _markerSubmitAttempts = 0;
      runMarkerTest();
    } else if (--_markerSubmitAttempts == 0) {
      IOLog("XenosMarker: SKIP no drained ring with space for one packet\n");
    }
  }

  // Let the marker run on its own refresh before submitting the resolve test.
  if (_clearSubmitAttempts != 0 && _markerSubmitAttempts == 0 && !markerWasPending) {
    if (!_markerScratch &&
        _ringBufferWritePtr <= kXenosRingbufferLength - kClearRingDwords &&
        readReg32(kXenosRegCpRbReadPtr) == _ringBufferWritePtr &&
        readReg32(kXenosRegCpRbWritePtr) == _ringBufferWritePtr) {
      _clearSubmitAttempts = 0;
      runClearTest();
    } else if (--_clearSubmitAttempts == 0) {
      IOLog("XenosClear: SKIP no drained ring available after marker test\n");
    }
  }

  // The restore is already queued. Keep the ring intact if the GPU stalled.
  if (_clearScratch && readReg32(kXenosRegCpRbReadPtr) != _ringBufferWritePtr) {
    _timerEventSource->setTimeoutMS(15);
    return;
  }

  writeRingPacket3(CP_SET_CONSTANT, 2);
  writeRing(CP_REG(REG_A2XX_RB_MODECONTROL));
  writeRing(A2XX_RB_MODECONTROL_EDRAM_MODE(COLOR_DEPTH));

  writeRingPacket3(CP_DRAW_INDX, 2);
  writeRing(0x00000000);
  writeRing((4 << 16) | DRAW(DI_PT_TRISTRIP, DI_SRC_SEL_AUTO_INDEX,
    INDEX_SIZE_IGN, IGNORE_VISIBILITY)); //xenos has NumIndices combined with the vgt_draw_initiator?

  writeRingPacket3(CP_SET_CONSTANT, 2);
  writeRing(CP_REG(REG_A2XX_RB_MODECONTROL));
  writeRing(A2XX_RB_MODECONTROL_EDRAM_MODE(EDRAM_COPY));

  drawResolveRectangle();

  syncRingBuffer();
  _timerEventSource->setTimeoutMS(15);
}

void XenosController::drawResolveRectangle() {
  writeRingPacket3(CP_DRAW_INDX, 2);
  writeRing(0x00000000);
  writeRing((3 << 16) | DRAW(DI_PT_RECTLIST, DI_SRC_SEL_AUTO_INDEX,
    INDEX_SIZE_IGN, IGNORE_VISIBILITY));
}

static inline void flushMarkerCacheLine(volatile UInt32 *slot) {
  asm volatile("dcbf 0,%0\n\tsync" : : "r"(slot) : "memory");
}

void XenosController::runMarkerTest() {
  IOBufferMemoryDescriptor *scratch =
      IOBufferMemoryDescriptor::withOptions(kIOMemoryPhysicallyContiguous, PAGE_SIZE, PAGE_SIZE);
  if (!scratch) {
    IOLog("XenosMarker: SKIP scratch allocation failed\n");
    return;
  }

  IOByteCount length = 0;
  const IOPhysicalAddress physical = scratch->getPhysicalSegment(0, &length);
  volatile UInt32 *slot = static_cast<volatile UInt32 *>(scratch->getBytesNoCopy());
  if (!slot || !physical || length < PAGE_SIZE || (physical & (PAGE_SIZE - 1)) ||
      physical > kDisplayMemoryStart - PAGE_SIZE) {
    IOLog("XenosMarker: SKIP scratch page is outside usable GPU RAM\n");
    scratch->release();
    return;
  }

  *slot = kMarkerInitial;
  flushMarkerCacheLine(slot);
  const UInt32 start = _ringBufferWritePtr;
  writeRingPacket3(CP_MEM_WRITE, 2);
  writeRing(static_cast<UInt32>(physical) | kMarkerSwap8In32);
  writeRing(kMarkerExpected);
  syncRingBuffer();

  UInt32 seen = kMarkerInitial;
  UInt32 polls = 0;
  for (; polls < kMarkerPollLimit; ++polls) {
    // Only the GPU writes this line after submission. Evict the CPU's
    // cached copy before reading the value back from memory.
    flushMarkerCacheLine(slot);
    seen = *slot;
    if (seen != kMarkerInitial)
      break;
    IODelay(kMarkerPollDelayUs);
  }

  IOLog("XenosMarker: %s MEM_WRITE expected=0x%08lx observed=0x%08lx "
        "packet=0x%04lx rptr=0x%04lx wptr=0x%04lx polls=%lu\n",
        seen == kMarkerExpected ? "PASS" : "FAIL",
        kMarkerExpected, seen, start, readReg32(kXenosRegCpRbReadPtr),
        readReg32(kXenosRegCpRbWritePtr), polls);
  if (seen == kMarkerExpected) {
    scratch->release();
  } else {
    // A delayed GPU write must still have an allocated destination.
    _markerScratch = scratch;
  }
}

void XenosController::runClearTest() {
  // A 1920x1080, single-sample colour surface uses at most 1632 tiles
  // starting at tile zero. This test starts at tile 2032 with depth disabled.
  if (!_fbWidth || !_fbHeight || _fbWidth > 1920 || _fbHeight > 1080) {
    IOLog("XenosClear: SKIP display layout outside the tested eDRAM range\n");
    return;
  }

  // Keep the throwaway first resolve, actual readback and fence on separate pages.
  const UInt32 scratchBytes = 2 * kClearBytes + PAGE_SIZE;
  IOBufferMemoryDescriptor *scratch = IOBufferMemoryDescriptor::withOptions(
      kIOMemoryPhysicallyContiguous, scratchBytes, PAGE_SIZE);
  if (!scratch) {
    IOLog("XenosClear: SKIP scratch allocation failed\n");
    return;
  }

  IOByteCount length = 0;
  const IOPhysicalAddress physical = scratch->getPhysicalSegment(0, &length);
  UInt32 *memory = static_cast<UInt32 *>(scratch->getBytesNoCopy());
  if (!memory || !physical || length < scratchBytes || (physical & (PAGE_SIZE - 1)) ||
      physical > kDisplayMemoryStart - scratchBytes) {
    IOLog("XenosClear: SKIP scratch range is outside usable GPU RAM\n");
    scratch->release();
    return;
  }

  const UInt32 discard = static_cast<UInt32>(physical);
  const UInt32 readback = discard + kClearBytes;
  const UInt32 fence = readback + kClearBytes;
  UInt32 *pixelData = memory + kClearPixels;
  volatile UInt32 *pixels = pixelData;
  volatile UInt32 *marker = pixels + kClearPixels;
  for (UInt32 i = 0; i < kClearPixels; ++i)
    pixels[i] = kClearSentinel;
  *marker = kClearSentinel;
  for (UInt32 offset = 0; offset < kClearBytes; offset += CACHELINE_SIZE)
    flushMarkerCacheLine(pixels + offset / sizeof(UInt32));
  flushMarkerCacheLine(marker);

  const UInt32 savedClear = readReg32(4 * kXenosColorClear);
  const UInt32 savedClearLo = readReg32(4 * (kXenosColorClear + 1));
  const UInt32 savedCoherDest = readReg32(4 * kXenosCoherDestBase1);
  writeRingPacket0(kXenosWaitUntil, 1);
  writeRing(kWait3dIdleClean);
  writeRingPacket0(REG_A2XX_RB_SURFACE_INFO, 2);
  writeRing(kClearEdRamPitch);
  writeRing((k_8_8_8_8 << 16) | kClearTile);
  setGpuViewport(kClearSide, kClearSide);
  setGpuScissors(kClearSide, kClearSide);
  writeRingPacket0(REG_A2XX_RB_MODECONTROL, 1);
  writeRing(A2XX_RB_MODECONTROL_EDRAM_MODE(EDRAM_COPY));
  writeRingPacket0(kXenosColorClear, 2);
  writeRing(kClearColor);
  writeRing(kClearColor);

  // Xenos resolves the old contents first and applies the clear afterward.
  const UInt32 format = kResolveDisplayFormat | kMarkerSwap8In32;
  setResolveTarget(discard, kClearSide, kClearSide, kResolveConvert | kResolveColorClear, format);
  drawResolveRectangle();
  writeRingPacket0(kXenosWaitUntil, 1);
  writeRing(kWait3dIdleClean);
  setResolveTarget(readback, kClearSide, kClearSide, kResolveConvert, format);
  drawResolveRectangle();

  writeRingPacket3(CP_EVENT_WRITE, 1);
  writeRing(kCacheFlushEvent);
  writeRingPacket0(kXenosCoherDestBase1, 1);
  writeRing(0);
  writeRingPacket0(kXenosCoherStatusHost, 1);
  writeRing(kCoherResolveActions);
  writeRingPacket0(kXenosCoherSizeHost, 2);
  writeRing(kClearBytes);
  writeRing(readback);
  writeRingPacket3(CP_WAIT_REG_MEM, 5);
  writeRing(3); // Equal, register address.
  writeRing(kXenosCoherStatusHost);
  writeRing(0);
  writeRing(kCoherBusy);
  writeRing(8); // Poll interval.
  writeRingPacket0(kXenosWaitUntil, 1);
  writeRing(kWait3dIdleClean);

  writeRingPacket0(kXenosCoherDestBase1, 1);
  writeRing(savedCoherDest);
  writeRingPacket0(kXenosColorClear, 2);
  writeRing(savedClear);
  writeRing(savedClearLo);
  writeRingPacket0(REG_A2XX_RB_SURFACE_INFO, 2);
  writeRing(ALIGN(_fbWidth, 32));
  writeRing(k_8_8_8_8 << 16);
  setGpuViewport(_fbWidth, _fbHeight);
  setGpuScissors(_fbWidth, _fbHeight);
  setResolveTarget(_gpuPhysAddr, _fbWidth, _fbHeight, kResolveConvert, kResolveDisplayFormat);

  writeRingPacket3(CP_MEM_WRITE, 2);
  writeRing(fence | kMarkerSwap8In32);
  writeRing(kClearComplete);
  syncRingBuffer();

  UInt32 seen = kClearSentinel;
  for (UInt32 poll = 0; poll < kMarkerPollLimit; ++poll) {
    flushMarkerCacheLine(marker);
    seen = *marker;
    if (seen == kClearComplete)
      break;
    IODelay(kMarkerPollDelayUs);
  }
  if (seen != kClearComplete) {
    IOLog("XenosClear: FAIL fence=0x%08lx rptr=0x%04lx wptr=0x%04lx\n",
          seen, readReg32(kXenosRegCpRbReadPtr), readReg32(kXenosRegCpRbWritePtr));
    _clearScratch = scratch;
    return;
  }

  for (UInt32 offset = 0; offset < kClearBytes; offset += CACHELINE_SIZE)
    flushMarkerCacheLine(pixels + offset / sizeof(UInt32));
  UInt32 matches = 0;
  UInt32 firstMismatch = kClearPixels;
  for (UInt32 i = 0; i < kClearPixels; ++i) {
    if (pixels[i] == kClearExpected)
      ++matches;
    else if (firstMismatch == kClearPixels)
      firstMismatch = i;
  }
  if (matches == kClearPixels) {
    const bool published = setProperty("XenosClearPixels", pixelData, kClearBytes);
    IOLog("XenosClear: PASS 32x32 pitch=%lu expected=0x%08lx observed=0x%08lx matched=%lu/%lu ioreg=%s\n",
          kClearEdRamPitch, kClearExpected, pixels[0], matches, kClearPixels, published ? "yes" : "no");
  } else {
    IOLog("XenosClear: FAIL 32x32 expected=0x%08lx matched=%lu/%lu first=(%lu,%lu) observed=0x%08lx\n",
          kClearExpected, matches, kClearPixels, firstMismatch % kClearSide,
          firstMismatch / kClearSide, pixels[firstMismatch]);
  }
  scratch->release();
}

//
// Sync data to memory.
//
void XenosController::syncData(volatile void *data, UInt32 length) {
  if (length == 0) {
    return;
  }

  uintptr_t a    = (uintptr_t)data;
  uintptr_t base = a & ~((uintptr_t)CACHELINE_SIZE - 1);
  size_t    span = length + (size_t)(a - base);
  size_t    n    = (span + CACHELINE_SIZE - 1) / CACHELINE_SIZE;

  while (n--) {
    asm volatile("dcbst 0,%0" : : "r"(base) : "memory");
    base += CACHELINE_SIZE;
  }

  asm volatile("sync" ::: "memory");
}

//
// Syncs and updates the ring buffer.
//
void XenosController::syncRingBuffer(void) {
	syncData(_ringBuffer, kXenosRingbufferLength * 4);
	writeReg32(kXenosRegCpRbWritePtr, _ringBufferWritePtr);
}
