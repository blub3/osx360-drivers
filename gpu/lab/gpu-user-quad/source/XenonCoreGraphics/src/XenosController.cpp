//
//  XenosController.cpp
//  Xbox 360 graphics controller
//
//  Copyright © 2026 John Davis. All rights reserved.
//

#include <IOKit/IOPlatformExpert.h>

#include "XenosController.hpp"
#include "XenosQuadClient.hpp"

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

static const UInt32 kQuadWaitLimit = 20; // Work-loop ticks, approximately 300 ms.
static const UInt32 kQuadCooldownTicks = 8;
static const UInt32 kQuadRingDwords = 256;
static const UInt32 kQuadScratchPages = 5;
static const UInt32 kQuadReadbackOffset = PAGE_SIZE;
static const UInt32 kQuadFenceOffset = 2 * PAGE_SIZE;
static const UInt32 kQuadGeometryOffset = 3 * PAGE_SIZE;
static const UInt32 kQuadTextureOffset = 4 * PAGE_SIZE;
static const UInt32 kClearSide = 32;
static const UInt32 kClearEdRamPitch = 160; // Two 80-pixel eDRAM tiles per row.
static const UInt32 kClearPixels = kClearSide * kClearSide;
static const UInt32 kClearTile = 0x7f0;
static const UInt32 kClearColor = 0xff37b24c;  // Packed eDRAM colour, red in the low byte.
static const UInt32 kClearExpected = 0xff4cb237; // Resolve swaps the red and blue bytes.
static const UInt32 kClearSentinel = 0xa5a5a5a5;
static const UInt16 kXenosWaitUntil = 0x05c8;
static const UInt32 kWait3dIdleClean = 1 << 17;
static const UInt16 kXenosCoherDestBase1 = 0x2007;
static const UInt16 kXenosCoherSizeHost = 0x0a2f;
static const UInt16 kXenosCoherStatusHost = 0x0a31;
static const UInt32 kCoherVertexCache = 1 << 24;
static const UInt32 kCoherVertexAndTextureCaches = kCoherVertexCache | (1 << 25);
static const UInt32 kCoherResolveActions = kCoherVertexAndTextureCaches | 0x00000100;
static const UInt32 kCoherBusy = 1U << 31;
static const UInt16 kXenosColorClear = 0x231e;
static const UInt32 kResolveColorClear = 1 << 8;
static const UInt32 kCacheFlushEvent = 6;
static const UInt32 kQuadSide = 16;
static const UInt32 kQuadOrigin = 4;

// Bottom-left, bottom-right, top-left, top-right. Sample the 16x16
// upper-left crop of a 32x32 texture with point filtering.
static const float kQuadPositions[] = {
  -0.75f, -0.25f, 0.0f,  0.25f, -0.25f, 0.0f,
  -0.75f,  0.75f, 0.0f,  0.25f,  0.75f, 0.0f,
};
static const float kQuadUVs[] = {
  0.0f, 0.5f,  0.5f, 0.5f,
  0.0f, 0.0f,  0.5f, 0.0f,
};

static UInt32 quadTexel(UInt32 texture, UInt32 x, UInt32 y) {
  const UInt32 column = (x / 4) & 3;
  const UInt32 row = (y / 4) & 3;
  const UInt32 red = 176 + 12 * column;
  const UInt32 green = 32 + 20 * row;
  const UInt32 blue = 24 + 8 * (column ^ row);
  return 0xff000000U | (green << 8) |
         (texture == 0 ? (red << 16) | blue : (blue << 16) | red);
}

// Word index within a single 32x32, 32-bit Xenos resolve tile.
// Rows select the bank and flip the high pipe bit at y=8 and y=24.
// Reference: Xenia's Tiled2D and TiledCombine in src/xenia/gpu/texture_address.h.
static UInt32 resolveTileWord(UInt32 x, UInt32 y) {
  const UInt32 pipe = (x >> 3) ^ ((y & 8) >> 2);
  const UInt32 rows = (y & 30) << 5;
  const UInt32 withinPipe = ((x & 4) << 1) | ((y & 1) << 2) | (x & 3);
  return rows | (pipe << 4) | withinPipe;
}

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
  _quadClient = NULL;
  _quadScratch = NULL;
  _quadTicket = _quadTexture = _quadWait = _quadCooldown = 0;
  _quadGate = IOCommandGate::commandGate(this);
  if (_quadGate && _timerWorkLoop->addEventSource(_quadGate) != kIOReturnSuccess) {
    _quadGate->release();
    _quadGate = NULL;
  }
  if (_quadGate) {
    setProperty("IOUserClientClass", "XenosQuadClient");
    setProperty("XenosUserQuadResult", "Idle");
  }
  _timerEventSource->setTimeoutMS(1000);
  registerService();
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

  if (_markerSubmitAttempts) {
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

  if (_quadCooldown)
    --_quadCooldown;
  if (!stepQuad()) {
    sender->setTimeoutMS(15);
    return;
  }

  setGpuEdRamMode(COLOR_DEPTH);

  writeRingPacket3(CP_DRAW_INDX, 2);
  writeRing(0x00000000);
  writeRing((4 << 16) | DRAW(DI_PT_TRISTRIP, DI_SRC_SEL_AUTO_INDEX,
    INDEX_SIZE_IGN, IGNORE_VISIBILITY)); //xenos has NumIndices combined with the vgt_draw_initiator?

  setGpuEdRamMode(EDRAM_COPY);

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

IOReturn XenosController::queueQuad(OSObject *owner, void *clientArg, void *textureArg,
                                      void *output, void *) {
  XenosController *self = static_cast<XenosController *>(owner);
  XenosQuadClient *client = static_cast<XenosQuadClient *>(clientArg);
  const UInt32 texture = static_cast<UInt32>(reinterpret_cast<uintptr_t>(textureArg));
  if (texture >= kXenosQuadTextureCount)
    return kIOReturnBadArgument;
  if (self->isInactive() || client->isInactive())
    return kIOReturnNotReady;
  if (self->_quadClient || self->_quadScratch || self->_quadCooldown)
    return kIOReturnBusy;

  if (++self->_quadTicket == 0)
    ++self->_quadTicket;
  self->_quadClient = client;
  client->retain();
  self->_quadTexture = texture;
  self->_quadWait = 0;
  client->_ticket = self->_quadTicket;
  bzero(client->_result, sizeof(client->_result));
  *static_cast<UInt32 *>(output) = self->_quadTicket;

  self->setProperty("XenosUserQuadTicket", self->_quadTicket, 32);
  self->setProperty("XenosUserQuadTexture", texture, 32);
  self->setProperty("XenosUserQuadResult", "Pending");
  return kIOReturnSuccess;
}

IOReturn XenosController::readQuadStatus(OSObject *owner, void *clientArg, void *ticketArg,
                                           void *output, void *) {
  XenosQuadClient *client = static_cast<XenosQuadClient *>(clientArg);
  const UInt32 ticket = static_cast<UInt32>(reinterpret_cast<uintptr_t>(ticketArg));
  if (static_cast<XenosController *>(owner)->isInactive() || client->isInactive())
    return kIOReturnNotReady;
  if (!ticket || ticket != client->_ticket)
    return kIOReturnBadArgument;
  memcpy(output, client->_result, sizeof(client->_result));
  return kIOReturnSuccess;
}

bool XenosController::stepQuad() {
  if (!_quadClient && !_quadScratch)
    return true;
  const bool drained = readReg32(kXenosRegCpRbReadPtr) == _ringBufferWritePtr &&
                       readReg32(kXenosRegCpRbWritePtr) == _ringBufferWritePtr;

  if (!_quadScratch) {
    if (!drained || _markerSubmitAttempts) {
      if (++_quadWait >= kQuadWaitLimit) {
        finishQuad(kXenosQuadUnavailable, 0, 0, 0);
        return true;
      }
      return false;
    }
    if (_ringBufferWritePtr > kXenosRingbufferLength - kQuadRingDwords) {
      while (_ringBufferWritePtr)
        writeRing(2U << 30); // Type-2 NOP up to slot zero.
      syncRingBuffer();
      return false;
    }
    if (!submitQuad()) {
      finishQuad(kXenosQuadUnavailable, 0, 0, 0);
      return true;
    }
    _quadWait = 0;
    return false;
  }

  UInt8 *memory = static_cast<UInt8 *>(_quadScratch->getBytesNoCopy());
  volatile UInt32 *slot = reinterpret_cast<volatile UInt32 *>(memory + kQuadFenceOffset);
  flushMarkerCacheLine(slot);
  const UInt32 fence = *slot;
  if (!drained || fence != _quadTicket) {
    if (_quadClient && ++_quadWait >= kQuadWaitLimit)
      finishQuad(kXenosQuadTimedOut, fence, 0, 0);
    // The batch may still write to scratch. Keep it wired and hold new
    // submissions until the fence and the display-state restore have landed.
    return false;
  }

  if (_quadClient) {
    volatile UInt32 *pixels = reinterpret_cast<volatile UInt32 *>(memory + kQuadReadbackOffset);
    for (UInt32 offset = 0; offset < PAGE_SIZE; offset += CACHELINE_SIZE)
      flushMarkerCacheLine(pixels + offset / sizeof(UInt32));

    UInt32 mismatches = 0;
    for (UInt32 y = 0; y < kClearSide; ++y) {
      for (UInt32 x = 0; x < kClearSide; ++x) {
        const UInt32 dx = x - kQuadOrigin;
        const UInt32 dy = y - kQuadOrigin;
        const UInt32 expected = dx < kQuadSide && dy < kQuadSide
                              ? quadTexel(_quadTexture, dx, dy) : kClearExpected;
        mismatches += pixels[resolveTileWord(x, y)] != expected;
      }
    }
    finishQuad(mismatches ? kXenosQuadPixelsFailed : kXenosQuadPassed,
               fence, kClearPixels, mismatches);
  }
  _quadScratch->release();
  _quadScratch = NULL;
  return true;
}

void XenosController::finishQuad(UInt32 state, UInt32 fence, UInt32 checked, UInt32 mismatches) {
  XenosQuadClient *client = _quadClient;
  client->_result[0] = state;
  client->_result[1] = fence;
  client->_result[2] = checked;
  client->_result[3] = mismatches;
  _quadClient = NULL;
  _quadCooldown = kQuadCooldownTicks;
  client->release();

  const char *result = state == kXenosQuadPassed ? "PASS" :
                       state == kXenosQuadPixelsFailed ? "FAIL pixels" :
                       state == kXenosQuadTimedOut ? "FAIL timeout" : "FAIL unavailable";
  setProperty("XenosUserQuadFence", fence, 32);
  setProperty("XenosUserQuadPixelsChecked", checked, 32);
  setProperty("XenosUserQuadMismatches", mismatches, 32);
  setProperty("XenosUserQuadResult", result);
  IOLog("XenosUserQuad: %s ticket=%lu texture=%lu fence=%lu pixels=%lu mismatches=%lu\n",
        result, _quadTicket, _quadTexture, fence, checked, mismatches);
}

bool XenosController::submitQuad() {
  // Use spare eDRAM tiles beyond the largest supported desktop surface.
  if (!_fbWidth || !_fbHeight || _fbWidth > 1920 || _fbHeight > 1080)
    return false;

  const UInt32 scratchBytes = kQuadScratchPages * PAGE_SIZE;
  IOBufferMemoryDescriptor *scratch = IOBufferMemoryDescriptor::withOptions(
      kIOMemoryPhysicallyContiguous, scratchBytes, PAGE_SIZE);
  if (!scratch)
    return false;

  IOByteCount length = 0;
  const IOPhysicalAddress physical = scratch->getPhysicalSegment(0, &length);
  UInt8 *memory = static_cast<UInt8 *>(scratch->getBytesNoCopy());
  if (!memory || !physical || length < scratchBytes || (physical & (PAGE_SIZE - 1)) ||
      physical > kDisplayMemoryStart - scratchBytes) {
    scratch->release();
    return false;
  }
  _quadScratch = scratch;

  const UInt32 base = static_cast<UInt32>(physical);
  const UInt32 readback = base + kQuadReadbackOffset;
  const UInt32 fence = base + kQuadFenceOffset;
  const UInt32 vertices = base + kQuadGeometryOffset;
  const UInt32 uv = vertices + CACHELINE_SIZE;
  const UInt32 texture = base + kQuadTextureOffset;
  memcpy(memory + kQuadGeometryOffset, kQuadPositions, sizeof(kQuadPositions));
  memcpy(memory + kQuadGeometryOffset + CACHELINE_SIZE, kQuadUVs, sizeof(kQuadUVs));
  UInt32 *texels = reinterpret_cast<UInt32 *>(memory + kQuadTextureOffset);
  for (UInt32 y = 0; y < kClearSide; ++y)
    for (UInt32 x = 0; x < kClearSide; ++x)
      texels[resolveTileWord(x, y)] = quadTexel(_quadTexture, x, y);

  syncData(memory + kQuadGeometryOffset, CACHELINE_SIZE + sizeof(kQuadUVs));
  syncData(texels, PAGE_SIZE);
  volatile UInt32 *pixels = reinterpret_cast<volatile UInt32 *>(memory + kQuadReadbackOffset);
  for (UInt32 i = 0; i < kClearPixels; ++i)
    pixels[i] = kClearSentinel;
  for (UInt32 offset = 0; offset < PAGE_SIZE; offset += CACHELINE_SIZE)
    flushMarkerCacheLine(pixels + offset / sizeof(UInt32));
  volatile UInt32 *marker = reinterpret_cast<volatile UInt32 *>(memory + kQuadFenceOffset);
  *marker = 0;
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
  setGpuEdRamMode(EDRAM_COPY);
  writeRingPacket0(kXenosColorClear, 2);
  writeRing(kClearColor);
  writeRing(kClearColor);

  const UInt32 format = kResolveDisplayFormat | kMarkerSwap8In32;
  setResolveTarget(base, kClearSide, kClearSide, kResolveConvert | kResolveColorClear, format);
  drawResolveRectangle();
  writeRingPacket0(kXenosWaitUntil, 1);
  writeRing(kWait3dIdleClean);

  syncGpuMemory(vertices, 2 * PAGE_SIZE, kCoherVertexAndTextureCaches);
  writeRingPacket0(REG_A2XX_RB_COPY_CONTROL, 1);
  writeRing(0);
  writeRingPacket3(CP_INVALIDATE_STATE, 1);
  writeRing(0x00000200);
  setGpuTexturePixelShader(true);
  setGpuEdRamMode(COLOR_DEPTH);
  drawTextureLayer(vertices, uv, texture);
  resolveQuadColor(readback);

  writeRingPacket3(CP_EVENT_WRITE, 1);
  writeRing(kCacheFlushEvent);
  writeRingPacket0(kXenosCoherDestBase1, 1);
  writeRing(0);
  syncGpuMemory(readback, PAGE_SIZE, kCoherResolveActions);
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
  writeRing(_quadTicket);
  syncRingBuffer();
  return true;
}

void XenosController::drawTextureLayer(UInt32 positions, UInt32 uv, UInt32 texture) {
  setGpuVertices(positions, sizeof(kQuadPositions), uv, sizeof(kQuadUVs));
  setGpuTexture(texture, kClearSide, kClearSide, SQ_TEX_FILTER_POINT, true);
  writeRingPacket3(CP_DRAW_INDX, 2);
  writeRing(0);
  writeRing((4 << 16) | DRAW(DI_PT_TRISTRIP, DI_SRC_SEL_AUTO_INDEX,
    INDEX_SIZE_IGN, IGNORE_VISIBILITY));
  writeRingPacket0(kXenosWaitUntil, 1);
  writeRing(kWait3dIdleClean);
}

void XenosController::resolveQuadColor(UInt32 address) {
  syncGpuMemory(_triagPhys, PAGE_SIZE, kCoherVertexCache);
  setGpuDisplayVertices();
  setGpuTexture(_fbPhysAddr, _fbWidth, _fbHeight, SQ_TEX_FILTER_BILINEAR, false);
  writeRingPacket3(CP_INVALIDATE_STATE, 1);
  writeRing(0x00000200);
  setGpuTexturePixelShader(false);
  setGpuEdRamMode(EDRAM_COPY);
  setResolveTarget(address, kClearSide, kClearSide, kResolveConvert,
                   kResolveDisplayFormat | kMarkerSwap8In32);
  drawResolveRectangle();
  writeRingPacket0(kXenosWaitUntil, 1);
  writeRing(kWait3dIdleClean);
}

void XenosController::syncGpuMemory(UInt32 address, UInt32 bytes, UInt32 actions) {
  writeRingPacket0(kXenosCoherStatusHost, 1);
  writeRing(actions);
  writeRingPacket0(kXenosCoherSizeHost, 2);
  writeRing(bytes);
  writeRing(address);
  writeRingPacket3(CP_WAIT_REG_MEM, 5);
  writeRing(3); // Equal, register address.
  writeRing(kXenosCoherStatusHost);
  writeRing(0);
  writeRing(kCoherBusy);
  writeRing(8); // Poll interval.
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
