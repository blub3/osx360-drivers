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

static const UInt32 kCompositeSubmitAttempts = 16;
static const UInt32 kCompositeRingDwords = 384; // Includes display state restore and the refresh draw.
static const UInt32 kClearSide = 32;
static const UInt32 kClearEdRamPitch = 160; // Two 80-pixel eDRAM tiles per row.
static const UInt32 kClearPixels = kClearSide * kClearSide;
static const UInt32 kClearBytes = kClearPixels * sizeof(UInt32);
static const UInt32 kClearTile = 0x7f0;
static const UInt32 kClearColor = 0xff37b24c;  // Packed eDRAM colour, red in the low byte.
static const UInt32 kClearExpected = 0xff4cb237; // Resolve swaps the red and blue bytes.
static const UInt32 kClearSentinel = 0xa5a5a5a5;
static const UInt32 kCompositeComplete = 0x54434f4d;
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
static const int kBlendTolerance = 1;
static const UInt32 kLayerSide = 16;
static const UInt32 kBackOrigin = 4;
static const UInt32 kFrontOrigin = 12;
static const UInt32 kLowAlpha = 64;
static const UInt32 kHighAlpha = 192;

// Bottom-left, bottom-right, top-left, top-right. Both strips sample a
// 16x16 crop in the top-left of their respective 32x32 textures.
static const float kBackPositions[] = {
  -0.75f, -0.25f, 0.0f,  0.25f, -0.25f, 0.0f,
  -0.75f,  0.75f, 0.0f,  0.25f,  0.75f, 0.0f,
};
static const float kFrontPositions[] = {
  -0.25f, -0.75f, 0.0f,  0.75f, -0.75f, 0.0f,
  -0.25f,  0.25f, 0.0f,  0.75f,  0.25f, 0.0f,
};
static const float kLayerUVs[] = {
  0.0f, 0.5f,  0.5f, 0.5f,
  0.0f, 0.0f,  0.5f, 0.0f,
};

static UInt32 backTexel(UInt32 x, UInt32 y) {
  const UInt32 column = (x / 4) & 3;
  const UInt32 row = (y / 4) & 3;
  return 0xff000000U | ((176 + 12 * column) << 16) |
         ((32 + 20 * row) << 8) | (24 + 8 * (column ^ row));
}

static UInt32 frontTexel(UInt32 x, UInt32 y) {
  const UInt32 column = (x / 4) & 3;
  const UInt32 row = (y / 4) & 3;
  const UInt32 alpha = ((column ^ row) & 1) ? kHighAlpha : kLowAlpha;
  return (alpha << 24) | ((20 + 9 * row) << 16) |
         ((96 + 16 * column) << 8) | (160 + 18 * ((column + 2 * row) & 3));
}

static bool layerCovers(UInt32 x, UInt32 y, UInt32 origin) {
  return x >= origin && x < origin + kLayerSide &&
         y >= origin && y < origin + kLayerSide;
}

static UInt32 textureSourceOver(UInt32 source, UInt32 destination) {
  const UInt32 alpha = source >> 24;
  // Both the back texture and the clear color have opaque alpha.
  UInt32 result = 0xff000000U;
  for (unsigned shift = 0; shift < 24; shift += 8) {
    const UInt32 src = (source >> shift) & 0xff;
    const UInt32 dst = (destination >> shift) & 0xff;
    result |= ((alpha * src + (255 - alpha) * dst + 127) / 255) << shift;
  }
  return result;
}

static bool compositePixelMatches(UInt32 observed, UInt32 expected, bool allowRounding) {
  if (!allowRounding)
    return observed == expected;

  for (unsigned shift = 0; shift < 32; shift += 8) {
    const int delta = static_cast<int>((observed >> shift) & 0xff) -
                      static_cast<int>((expected >> shift) & 0xff);
    if (delta < -kBlendTolerance || delta > kBlendTolerance)
      return false;
  }
  return true;
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
  _compositeSubmitAttempts = kCompositeSubmitAttempts;
  _compositeScratch = NULL;
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

  // Let the marker run on its own refresh before compositing the layers.
  if (_compositeSubmitAttempts != 0 && _markerSubmitAttempts == 0 && !markerWasPending) {
    if (!_markerScratch &&
        _ringBufferWritePtr <= kXenosRingbufferLength - kCompositeRingDwords &&
        readReg32(kXenosRegCpRbReadPtr) == _ringBufferWritePtr &&
        readReg32(kXenosRegCpRbWritePtr) == _ringBufferWritePtr) {
      _compositeSubmitAttempts = 0;
      runTextureCompositeTest();
    } else if (--_compositeSubmitAttempts == 0) {
      IOLog("XenosTextureComposite: SKIP no drained ring available after marker test\n");
    }
  }

  // The restore is already queued. Keep the ring intact if the GPU stalled.
  if (_compositeScratch && readReg32(kXenosRegCpRbReadPtr) != _ringBufferWritePtr) {
    _timerEventSource->setTimeoutMS(15);
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

void XenosController::runTextureCompositeTest() {
  // A 1920x1080, single-sample colour surface uses at most 1632 tiles
  // starting at tile zero. The test reserves four private color tiles at 2032.
  if (!_fbWidth || !_fbHeight || _fbWidth > 1920 || _fbHeight > 1080) {
    IOLog("XenosTextureComposite: SKIP display layout outside the tested eDRAM range\n");
    return;
  }

  // Discard, blend-on readback, blend-off readback, fence, geometry, two tiled textures.
  const UInt32 scratchBytes = 7 * PAGE_SIZE;
  IOBufferMemoryDescriptor *scratch = IOBufferMemoryDescriptor::withOptions(
      kIOMemoryPhysicallyContiguous, scratchBytes, PAGE_SIZE);
  if (!scratch) {
    IOLog("XenosTextureComposite: SKIP scratch allocation failed\n");
    return;
  }

  IOByteCount length = 0;
  const IOPhysicalAddress physical = scratch->getPhysicalSegment(0, &length);
  UInt32 *memory = static_cast<UInt32 *>(scratch->getBytesNoCopy());
  if (!memory || !physical || length < scratchBytes || (physical & (PAGE_SIZE - 1)) ||
      physical > kDisplayMemoryStart - scratchBytes) {
    IOLog("XenosTextureComposite: SKIP scratch range is outside usable GPU RAM\n");
    scratch->release();
    return;
  }

  const UInt32 discard = static_cast<UInt32>(physical);
  const UInt32 readback = discard + PAGE_SIZE;
  const UInt32 control = readback + PAGE_SIZE;
  const UInt32 fence = control + PAGE_SIZE;
  const UInt32 vertexAddress = fence + PAGE_SIZE;
  const UInt32 frontPositionAddress = vertexAddress + CACHELINE_SIZE;
  const UInt32 uvAddress = vertexAddress + 2 * CACHELINE_SIZE;
  const UInt32 backTexture = vertexAddress + PAGE_SIZE;
  const UInt32 frontTexture = backTexture + PAGE_SIZE;
  UInt8 *bytes = reinterpret_cast<UInt8 *>(memory);
  volatile UInt32 *pixels = reinterpret_cast<volatile UInt32 *>(bytes + PAGE_SIZE);
  volatile UInt32 *controlPixels = reinterpret_cast<volatile UInt32 *>(bytes + 2 * PAGE_SIZE);
  volatile UInt32 *marker = reinterpret_cast<volatile UInt32 *>(bytes + 3 * PAGE_SIZE);
  UInt8 *vertices = bytes + 4 * PAGE_SIZE;
  UInt32 *back = reinterpret_cast<UInt32 *>(bytes + 5 * PAGE_SIZE);
  UInt32 *front = reinterpret_cast<UInt32 *>(bytes + 6 * PAGE_SIZE);
  memcpy(vertices, kBackPositions, sizeof(kBackPositions));
  memcpy(vertices + CACHELINE_SIZE, kFrontPositions, sizeof(kFrontPositions));
  memcpy(vertices + 2 * CACHELINE_SIZE, kLayerUVs, sizeof(kLayerUVs));
  for (UInt32 y = 0; y < kClearSide; ++y) {
    for (UInt32 x = 0; x < kClearSide; ++x) {
      const UInt32 i = resolveTileWord(x, y);
      back[i] = backTexel(x, y);
      front[i] = frontTexel(x, y);
    }
  }
  syncData(vertices, 2 * CACHELINE_SIZE + sizeof(kLayerUVs));
  syncData(back, 2 * PAGE_SIZE);
  for (UInt32 i = 0; i < kClearPixels; ++i) {
    pixels[i] = kClearSentinel;
    controlPixels[i] = kClearSentinel;
  }
  *marker = kClearSentinel;
  for (UInt32 offset = 0; offset < 2 * PAGE_SIZE; offset += CACHELINE_SIZE)
    flushMarkerCacheLine(pixels + offset / sizeof(UInt32));
  flushMarkerCacheLine(marker);

  const UInt32 savedClear = readReg32(4 * kXenosColorClear);
  const UInt32 savedClearLo = readReg32(4 * (kXenosColorClear + 1));
  const UInt32 savedCoherDest = readReg32(4 * kXenosCoherDestBase1);
  const UInt32 savedBlend = readReg32(4 * REG_A2XX_RB_BLEND_CONTROL);
  const UInt32 savedColorControl = readReg32(4 * REG_A2XX_RB_COLORCONTROL);
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

  // Resolve old color to a private discard page, then clear color.
  const UInt32 format = kResolveDisplayFormat | kMarkerSwap8In32;
  setResolveTarget(discard, kClearSide, kClearSide, kResolveConvert | kResolveColorClear, format);
  drawResolveRectangle();
  writeRingPacket0(kXenosWaitUntil, 1);
  writeRing(kWait3dIdleClean);

  syncGpuMemory(vertexAddress, 3 * PAGE_SIZE, kCoherVertexAndTextureCaches);
  writeRingPacket0(REG_A2XX_RB_COPY_CONTROL, 1);
  writeRing(0);
  writeRingPacket3(CP_INVALIDATE_STATE, 1);
  writeRing(0x00000200);
  setGpuTexturePixelShader(true);
  setGpuEdRamMode(COLOR_DEPTH);
  setBlendTestState(false);
  drawTextureLayer(vertexAddress, uvAddress, backTexture);
  setBlendTestState(true);
  drawTextureLayer(frontPositionAddress, uvAddress, frontTexture);

  setBlendTestState(false);
  resolveCompositeColor(readback);

  // The first resolve kept eDRAM intact. Draw the same front texture
  // with replacement factors, retaining its RGB and alpha in the control.
  writeRingPacket0(REG_A2XX_RB_COPY_CONTROL, 1);
  writeRing(0);
  writeRingPacket3(CP_INVALIDATE_STATE, 1);
  writeRing(0x00000200);
  setGpuTexturePixelShader(true);
  setGpuEdRamMode(COLOR_DEPTH);
  drawTextureLayer(frontPositionAddress, uvAddress, frontTexture);
  resolveCompositeColor(control);

  writeRingPacket3(CP_EVENT_WRITE, 1);
  writeRing(kCacheFlushEvent);
  writeRingPacket0(kXenosCoherDestBase1, 1);
  writeRing(0);
  syncGpuMemory(readback, 2 * PAGE_SIZE, kCoherResolveActions);
  writeRingPacket0(kXenosWaitUntil, 1);
  writeRing(kWait3dIdleClean);

  writeRingPacket0(kXenosCoherDestBase1, 1);
  writeRing(savedCoherDest);
  writeRingPacket0(kXenosColorClear, 2);
  writeRing(savedClear);
  writeRing(savedClearLo);
  writeRingPacket0(REG_A2XX_RB_BLEND_CONTROL, 2);
  writeRing(savedBlend);
  writeRing(savedColorControl);
  writeRingPacket0(REG_A2XX_RB_SURFACE_INFO, 2);
  writeRing(ALIGN(_fbWidth, 32));
  writeRing(k_8_8_8_8 << 16);
  setGpuViewport(_fbWidth, _fbHeight);
  setGpuScissors(_fbWidth, _fbHeight);
  setResolveTarget(_gpuPhysAddr, _fbWidth, _fbHeight, kResolveConvert, kResolveDisplayFormat);

  writeRingPacket3(CP_MEM_WRITE, 2);
  writeRing(fence | kMarkerSwap8In32);
  writeRing(kCompositeComplete);
  syncRingBuffer();

  UInt32 seen = kClearSentinel;
  for (UInt32 poll = 0; poll < kMarkerPollLimit; ++poll) {
    flushMarkerCacheLine(marker);
    seen = *marker;
    if (seen == kCompositeComplete)
      break;
    IODelay(kMarkerPollDelayUs);
  }
  if (seen != kCompositeComplete) {
    IOLog("XenosTextureComposite: FAIL fence=0x%08lx rptr=0x%04lx wptr=0x%04lx\n",
          seen, readReg32(kXenosRegCpRbReadPtr), readReg32(kXenosRegCpRbWritePtr));
    _compositeScratch = scratch;
    return;
  }

  for (UInt32 offset = 0; offset < 2 * PAGE_SIZE; offset += CACHELINE_SIZE)
    flushMarkerCacheLine(pixels + offset / sizeof(UInt32));

  // Publish each resolved image in row order. The discard page can be reused
  // after the fence, and unfamiliar values are kept out of the register dump.
  const char *const properties[] = { "XenosTextureCompositePixels", "XenosTextureControlPixels" };
  volatile UInt32 *const images[] = { pixels, controlPixels };
  UInt32 passes = 0;
  for (UInt32 pass = 0; pass < 2; ++pass) {
    const bool blending = pass == 0;
    UInt32 mismatches = 0, unexpected = 0, untouched = 0;
    UInt32 overlapLow = 0, overlapHigh = 0, frontOnly = 0, backOnly = 0, background = 0;
    UInt32 firstMismatch = kClearPixels, firstExpected = 0;
    for (UInt32 y = 0; y < kClearSide; ++y) {
      for (UInt32 x = 0; x < kClearSide; ++x) {
        const UInt32 i = y * kClearSide + x;
        const UInt32 observed = images[pass][resolveTileWord(x, y)];
        const bool onBack = layerCovers(x, y, kBackOrigin);
        const bool onFront = layerCovers(x, y, kFrontOrigin);
        const UInt32 dst = onBack ? backTexel(x - kBackOrigin, y - kBackOrigin) : kClearExpected;
        const UInt32 src = onFront ? frontTexel(x - kFrontOrigin, y - kFrontOrigin) : dst;
        const UInt32 blended = onFront ? textureSourceOver(src, dst) : dst;
        const UInt32 expected = blending ? blended : src;
        memory[i] = observed;
        untouched += observed == kClearSentinel;
        // Publish readbacks that contain only fixture values, including a
        // dropped layer or an unexpected replacement instead of blending.
        unexpected += observed != kClearSentinel && observed != kClearExpected &&
                      observed != dst && observed != src &&
                      !compositePixelMatches(observed, blended, onFront);
        if (compositePixelMatches(observed, expected, blending && onFront)) {
          overlapLow += onBack && onFront && (src >> 24) == kLowAlpha;
          overlapHigh += onBack && onFront && (src >> 24) == kHighAlpha;
          frontOnly += onFront && !onBack;
          backOnly += onBack && !onFront;
          background += !onBack && !onFront;
        } else {
          if (mismatches == 0) {
            firstMismatch = i;
            firstExpected = expected;
          }
          ++mismatches;
        }
      }
    }
    const bool published = unexpected == 0 &&
                           setProperty(properties[pass], memory, kClearBytes);
    IOLog("XenosTextureComposite: blend=%s overlap_low=%lu overlap_high=%lu front_only=%lu "
          "back_only=%lu background=%lu untouched=%lu unexpected=%lu mismatches=%lu ioreg=%s\n",
          blending ? "on" : "off", overlapLow, overlapHigh, frontOnly, backOnly,
          background, untouched, unexpected, mismatches, published ? "yes" : "no");
    if (mismatches == 0) {
      ++passes;
    } else {
      IOLog("XenosTextureComposite: blend=%s first mismatch=(%lu,%lu) expected=0x%08lx observed=0x%08lx\n",
            blending ? "on" : "off", firstMismatch % kClearSide, firstMismatch / kClearSide,
            firstExpected, memory[firstMismatch]);
    }
  }
  IOLog("XenosTextureComposite: %s 32x32 blend and control matched=%lu/2 tolerance=%d\n",
        passes == 2 ? "PASS" : "FAIL", passes, kBlendTolerance);
  scratch->release();
}

void XenosController::drawTextureLayer(UInt32 positions, UInt32 uv, UInt32 texture) {
  setGpuVertices(positions, sizeof(kBackPositions), uv, sizeof(kLayerUVs));
  setGpuTexture(texture, kClearSide, kClearSide, SQ_TEX_FILTER_POINT, true);
  writeRingPacket3(CP_DRAW_INDX, 2);
  writeRing(0);
  writeRing((4 << 16) | DRAW(DI_PT_TRISTRIP, DI_SRC_SEL_AUTO_INDEX,
    INDEX_SIZE_IGN, IGNORE_VISIBILITY));
  writeRingPacket0(kXenosWaitUntil, 1);
  writeRing(kWait3dIdleClean);
}

void XenosController::resolveCompositeColor(UInt32 address) {
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

void XenosController::setBlendTestState(bool enabled) {
  // RGB uses straight-alpha source-over. Alpha uses source plus retained
  // destination, and both combine operations are ADD (zero).
  const UInt32 blend = enabled
    ? A2XX_RB_BLEND_CONTROL_COLOR_SRCBLEND(FACTOR_SRC_ALPHA) |
      A2XX_RB_BLEND_CONTROL_COLOR_DESTBLEND(FACTOR_ONE_MINUS_SRC_ALPHA) |
      A2XX_RB_BLEND_CONTROL_ALPHA_SRCBLEND(FACTOR_ONE) |
      A2XX_RB_BLEND_CONTROL_ALPHA_DESTBLEND(FACTOR_ONE_MINUS_SRC_ALPHA)
    : A2XX_RB_BLEND_CONTROL_COLOR_SRCBLEND(FACTOR_ONE) |
      A2XX_RB_BLEND_CONTROL_ALPHA_SRCBLEND(FACTOR_ONE);
  writeRingPacket0(REG_A2XX_RB_BLEND_CONTROL, 2);
  writeRing(blend);
  writeRing(A2XX_RB_COLORCONTROL_ALPHA_FUNC(FUNC_ALWAYS));
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
