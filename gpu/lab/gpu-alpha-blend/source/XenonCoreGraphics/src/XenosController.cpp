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

static const UInt32 kTriangleSubmitAttempts = 16;
static const UInt32 kTriangleRingDwords = 416; // Includes display state restore and the refresh draw.
static const UInt32 kClearSide = 32;
static const UInt32 kClearEdRamPitch = 160; // Two 80-pixel eDRAM tiles per row.
static const UInt32 kClearPixels = kClearSide * kClearSide;
static const UInt32 kClearBytes = kClearPixels * sizeof(UInt32);
static const UInt32 kClearTile = 0x7f0;
static const UInt32 kClearColor = 0xff37b24c;  // Packed eDRAM colour, red in the low byte.
static const UInt32 kClearExpected = 0xff4cb237; // Resolve swaps the red and blue bytes.
static const UInt32 kClearSentinel = 0xa5a5a5a5;
static const UInt32 kTriangleComplete = 0x424c4e44;
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
static const UInt32 kOpaqueRed = 0xffff0000;
static const UInt32 kQuarterBlue = 0x400000ff;
static const int kBlendTolerance = 1;

// The 32x32 viewport maps these to (4,4), (24,4), (4,24) and
// (8,8), (28,8), (8,28). Each vertex is float4 position then RGBA.
static const float kOpaqueTriangle[][8] = {
  { -0.75f,  0.75f, 0.0f, 1.0f,  1.0f, 0.0f, 0.0f, 1.0f },
  {  0.50f,  0.75f, 0.0f, 1.0f,  1.0f, 0.0f, 0.0f, 1.0f },
  { -0.75f, -0.50f, 0.0f, 1.0f,  1.0f, 0.0f, 0.0f, 1.0f },
};

static const float kTranslucentTriangle[][8] = {
  { -0.50f,  0.50f, 0.0f, 1.0f,  0.0f, 0.0f, 1.0f, 0.25f },
  {  0.75f,  0.50f, 0.0f, 1.0f,  0.0f, 0.0f, 1.0f, 0.25f },
  { -0.50f, -0.75f, 0.0f, 1.0f,  0.0f, 0.0f, 1.0f, 0.25f },
};

static bool coversBlendPixel(UInt32 x, UInt32 y, UInt32 corner) {
  // Samples are at half-pixel centers, with diagonal ties outside.
  return x >= corner && y >= corner && x + y + 1 < 2 * corner + 20;
}

static UInt32 blendBlueOver(UInt32 destination) {
  // Round 0.25 * (0,0,255) + 0.75 * destination to 8-bit RGB.
  // Source-over alpha remains opaque because the destination is opaque.
  const UInt32 red = (3 * ((destination >> 16) & 0xff) + 2) / 4;
  const UInt32 green = (3 * ((destination >> 8) & 0xff) + 2) / 4;
  const UInt32 blue = (255 + 3 * (destination & 0xff) + 2) / 4;
  return 0xff000000U | (red << 16) | (green << 8) | blue;
}

static UInt32 expectedBlendPixel(bool opaque, bool translucent, bool blending) {
  const UInt32 destination = opaque ? kOpaqueRed : kClearExpected;
  if (!translucent)
    return destination;
  return blending ? blendBlueOver(destination) : kQuarterBlue;
}

static bool blendPixelMatches(UInt32 observed, UInt32 expected) {
  if (expected == kOpaqueRed || expected == kClearExpected)
    return observed == expected;

  for (unsigned shift = 0; shift < 32; shift += 8) {
    const int delta = static_cast<int>((observed >> shift) & 0xff) -
                      static_cast<int>((expected >> shift) & 0xff);
    if (delta < -kBlendTolerance || delta > kBlendTolerance)
      return false;
  }
  return true;
}

static bool isBlendTestPixel(UInt32 color) {
  return color == kClearSentinel || color == kOpaqueRed || color == kClearExpected ||
         blendPixelMatches(color, kQuarterBlue) ||
         blendPixelMatches(color, blendBlueOver(kOpaqueRed)) ||
         blendPixelMatches(color, blendBlueOver(kClearExpected));
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
  _triangleSubmitAttempts = kTriangleSubmitAttempts;
  _triangleScratch = NULL;
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

  // Let the marker run on its own refresh before drawing the triangle.
  if (_triangleSubmitAttempts != 0 && _markerSubmitAttempts == 0 && !markerWasPending) {
    if (!_markerScratch &&
        _ringBufferWritePtr <= kXenosRingbufferLength - kTriangleRingDwords &&
        readReg32(kXenosRegCpRbReadPtr) == _ringBufferWritePtr &&
        readReg32(kXenosRegCpRbWritePtr) == _ringBufferWritePtr) {
      _triangleSubmitAttempts = 0;
      runBlendTest();
    } else if (--_triangleSubmitAttempts == 0) {
      IOLog("XenosBlendTriangles: SKIP no drained ring available after marker test\n");
    }
  }

  // The restore is already queued. Keep the ring intact if the GPU stalled.
  if (_triangleScratch && readReg32(kXenosRegCpRbReadPtr) != _ringBufferWritePtr) {
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

void XenosController::runBlendTest() {
  // A 1920x1080, single-sample colour surface uses at most 1632 tiles
  // starting at tile zero. The test reserves four private color tiles at 2032.
  if (!_fbWidth || !_fbHeight || _fbWidth > 1920 || _fbHeight > 1080) {
    IOLog("XenosBlendTriangles: SKIP display layout outside the tested eDRAM range\n");
    return;
  }

  // Discard, blend-on readback, blend-off readback, fence, then both triangles.
  const UInt32 scratchBytes = 5 * PAGE_SIZE;
  IOBufferMemoryDescriptor *scratch = IOBufferMemoryDescriptor::withOptions(
      kIOMemoryPhysicallyContiguous, scratchBytes, PAGE_SIZE);
  if (!scratch) {
    IOLog("XenosBlendTriangles: SKIP scratch allocation failed\n");
    return;
  }

  IOByteCount length = 0;
  const IOPhysicalAddress physical = scratch->getPhysicalSegment(0, &length);
  UInt32 *memory = static_cast<UInt32 *>(scratch->getBytesNoCopy());
  if (!memory || !physical || length < scratchBytes || (physical & (PAGE_SIZE - 1)) ||
      physical > kDisplayMemoryStart - scratchBytes) {
    IOLog("XenosBlendTriangles: SKIP scratch range is outside usable GPU RAM\n");
    scratch->release();
    return;
  }

  const UInt32 discard = static_cast<UInt32>(physical);
  const UInt32 readback = discard + PAGE_SIZE;
  const UInt32 control = readback + PAGE_SIZE;
  const UInt32 fence = control + PAGE_SIZE;
  const UInt32 vertexAddress = fence + PAGE_SIZE;
  UInt8 *bytes = reinterpret_cast<UInt8 *>(memory);
  volatile UInt32 *pixels = reinterpret_cast<volatile UInt32 *>(bytes + PAGE_SIZE);
  volatile UInt32 *controlPixels = reinterpret_cast<volatile UInt32 *>(bytes + 2 * PAGE_SIZE);
  volatile UInt32 *marker = reinterpret_cast<volatile UInt32 *>(bytes + 3 * PAGE_SIZE);
  UInt8 *vertices = bytes + 4 * PAGE_SIZE;
  memcpy(vertices, kOpaqueTriangle, sizeof(kOpaqueTriangle));
  memcpy(vertices + sizeof(kOpaqueTriangle), kTranslucentTriangle, sizeof(kTranslucentTriangle));
  syncData(vertices, sizeof(kOpaqueTriangle) + sizeof(kTranslucentTriangle));
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

  syncGpuMemory(vertexAddress, PAGE_SIZE, kCoherVertexCache);
  writeRingPacket0(REG_A2XX_RB_COPY_CONTROL, 1);
  writeRing(0);
  setGpuColorTriangleShaders();
  setGpuEdRamMode(COLOR_DEPTH);
  setBlendTestState(false);
  drawBlendTestTriangle(vertexAddress, sizeof(kOpaqueTriangle));
  const UInt32 translucentAddress = vertexAddress + sizeof(kOpaqueTriangle);
  setBlendTestState(true);
  drawBlendTestTriangle(translucentAddress, sizeof(kTranslucentTriangle));

  setBlendTestState(false);
  resolveBlendTestColor(readback);

  // The resolve kept eDRAM intact. Redraw the same translucent triangle
  // with replacement factors, to read back the unblended source color.
  writeRingPacket0(REG_A2XX_RB_COPY_CONTROL, 1);
  writeRing(0);
  setGpuColorTriangleShaders();
  setGpuEdRamMode(COLOR_DEPTH);
  drawBlendTestTriangle(translucentAddress, sizeof(kTranslucentTriangle));
  resolveBlendTestColor(control);

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
  writeRing(kTriangleComplete);
  syncRingBuffer();

  UInt32 seen = kClearSentinel;
  for (UInt32 poll = 0; poll < kMarkerPollLimit; ++poll) {
    flushMarkerCacheLine(marker);
    seen = *marker;
    if (seen == kTriangleComplete)
      break;
    IODelay(kMarkerPollDelayUs);
  }
  if (seen != kTriangleComplete) {
    IOLog("XenosBlendTriangles: FAIL fence=0x%08lx rptr=0x%04lx wptr=0x%04lx\n",
          seen, readReg32(kXenosRegCpRbReadPtr), readReg32(kXenosRegCpRbWritePtr));
    _triangleScratch = scratch;
    return;
  }

  for (UInt32 offset = 0; offset < 2 * PAGE_SIZE; offset += CACHELINE_SIZE)
    flushMarkerCacheLine(pixels + offset / sizeof(UInt32));

  // Publish each resolved image in row order. The discard page can be reused
  // after the fence, and unfamiliar values are kept out of the register dump.
  const char *const properties[] = { "XenosBlendTrianglePixels", "XenosBlendControlPixels" };
  volatile UInt32 *const images[] = { pixels, controlPixels };
  UInt32 passes = 0;
  for (UInt32 pass = 0; pass < 2; ++pass) {
    const bool blending = pass == 0;
    UInt32 mismatches = 0, unexpected = 0, untouched = 0;
    UInt32 overlapMatches = 0, overBackgroundMatches = 0;
    UInt32 firstMismatch = kClearPixels;
    for (UInt32 y = 0; y < kClearSide; ++y) {
      for (UInt32 x = 0; x < kClearSide; ++x) {
        const UInt32 i = y * kClearSide + x;
        const UInt32 observed = images[pass][resolveTileWord(x, y)];
        const bool opaque = coversBlendPixel(x, y, 4);
        const bool translucent = coversBlendPixel(x, y, 8);
        const UInt32 expected = expectedBlendPixel(opaque, translucent, blending);
        memory[i] = observed;
        untouched += observed == kClearSentinel;
        unexpected += !isBlendTestPixel(observed);
        if (blendPixelMatches(observed, expected)) {
          overlapMatches += opaque && translucent;
          overBackgroundMatches += !opaque && translucent;
        } else {
          if (mismatches == 0)
            firstMismatch = i;
          ++mismatches;
        }
      }
    }
    const bool published = unexpected == 0 &&
                           setProperty(properties[pass], memory, kClearBytes);
    IOLog("XenosBlendTriangles: blend=%s overlap=%lu over_background=%lu "
          "sample=0x%08lx untouched=%lu unexpected=%lu mismatches=%lu ioreg=%s\n",
          blending ? "on" : "off", overlapMatches, overBackgroundMatches,
          memory[8 * kClearSide + 8], untouched, unexpected, mismatches, published ? "yes" : "no");
    if (mismatches != 0) {
      const UInt32 x = firstMismatch % kClearSide;
      const UInt32 y = firstMismatch / kClearSide;
      const UInt32 expected = expectedBlendPixel(coversBlendPixel(x, y, 4),
                                                 coversBlendPixel(x, y, 8), blending);
      IOLog("XenosBlendTriangles: blend=%s first mismatch=(%lu,%lu) expected=0x%08lx observed=0x%08lx\n",
            blending ? "on" : "off", x, y, expected, memory[firstMismatch]);
    } else {
      ++passes;
    }
  }
  IOLog("XenosBlendTriangles: %s 32x32 blend and control matched=%lu/2 tolerance=%d\n",
        passes == 2 ? "PASS" : "FAIL", passes, kBlendTolerance);
  scratch->release();
}

void XenosController::drawBlendTestTriangle(UInt32 address, UInt32 vertexBytes) {
  setGpuVertices(address, vertexBytes);
  writeRingPacket3(CP_DRAW_INDX, 2);
  writeRing(0);
  writeRing((3 << 16) | DRAW(DI_PT_TRILIST, DI_SRC_SEL_AUTO_INDEX,
    INDEX_SIZE_IGN, IGNORE_VISIBILITY));
  writeRingPacket0(kXenosWaitUntil, 1);
  writeRing(kWait3dIdleClean);
}

void XenosController::resolveBlendTestColor(UInt32 address) {
  syncGpuMemory(_triagPhys, PAGE_SIZE, kCoherVertexCache);
  setGpuVertices(_triagPhys, 4 * 3 * sizeof(float));
  writeRingPacket3(CP_INVALIDATE_STATE, 1);
  writeRing(0x00000300);
  setGpuDisplayShaders();
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
