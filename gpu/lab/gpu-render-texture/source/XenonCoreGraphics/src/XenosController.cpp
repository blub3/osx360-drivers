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

static const UInt32 kRenderTextureSubmitAttempts = 16;
static const UInt32 kRenderTextureRingDwords = 416; // Includes display state restore and the refresh draw.
static const UInt32 kClearSide = 32;
static const UInt32 kClearEdRamPitch = 160; // Two 80-pixel eDRAM tiles per row.
static const UInt32 kClearPixels = kClearSide * kClearSide;
static const UInt32 kClearBytes = kClearPixels * sizeof(UInt32);
static const UInt32 kClearTile = 0x7f0;
static const UInt32 kSampleTile = 0x7f8;
static const UInt32 kSampleClearColor = 0xff473421; // Packed eDRAM R in low byte.
static const UInt32 kSampleBackground = 0xff213447;
static const UInt32 kClearColor = 0xff37b24c;  // Packed eDRAM colour, red in the low byte.
static const UInt32 kClearExpected = 0xff4cb237; // Resolve swaps the red and blue bytes.
static const UInt32 kClearSentinel = 0xa5a5a5a5;
static const UInt32 kRenderTextureComplete = 0x52545458;
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
static const int kChannelTolerance = 2;

// Vertices are deliberately out of perimeter order: bottom-right, top-left,
// bottom-left, top-right. The shared edge runs from top-right to bottom-left.
static const float kIndexedQuadVertices[][8] = {
  {  0.75f, -0.75f, 0.0f, 1.0f,  1.0f, 0.0f, 0.0f, 1.0f },
  { -0.75f,  0.75f, 0.0f, 1.0f,  1.0f, 0.0f, 0.0f, 1.0f },
  { -0.75f, -0.75f, 0.0f, 1.0f,  0.0f, 0.0f, 1.0f, 1.0f },
  {  0.75f,  0.75f, 0.0f, 1.0f,  0.0f, 1.0f, 0.0f, 1.0f }
};
static const UInt16 kQuadIndices[] = { 1, 3, 2, 3, 0, 2 };
static const UInt32 kQuadIndexCount = sizeof(kQuadIndices) / sizeof(kQuadIndices[0]);
static const UInt32 kQuadVertexCount = sizeof(kIndexedQuadVertices) / sizeof(kIndexedQuadVertices[0]);
static const UInt32 kIndexDmaSwap8In16 = 1U << 30; // PPC stores each UInt16 big-endian.

// 24x24 screen quad. The UVs sample the 24x24 source interior 1:1 with
// its horizontal axis reversed. Order is bottom-left, bottom-right, top-left, top-right.
static const float kSamplePositions[] = {
  -0.75f, -0.75f, 0.0f,  0.75f, -0.75f, 0.0f,
  -0.75f,  0.75f, 0.0f,  0.75f,  0.75f, 0.0f,
};
static const float kSampleUVs[] = {
  0.875f, 0.875f,  0.125f, 0.875f,
  0.875f, 0.125f,  0.125f, 0.125f,
};

static bool insideQuad(UInt32 x, UInt32 y) {
  return x >= 4 && x < 28 && y >= 4 && y < 28;
}

static UInt32 expectedQuadColor(UInt32 x, UInt32 y) {
  if (!insideQuad(x, y))
    return kClearExpected;

  // Double the half-pixel coordinates measured from the top-left corner.
  // The piecewise color gradient also identifies which diagonal was drawn.
  const UInt32 x2 = 2 * (x - 4) + 1;
  const UInt32 y2 = 2 * (y - 4) + 1;
  const bool upper = x2 + y2 <= 48;
  const UInt32 redWeight = upper ? 48 - x2 - y2 : x2 + y2 - 48;
  const UInt32 greenWeight = upper ? x2 : 48 - y2;
  const UInt32 blueWeight = upper ? y2 : 48 - x2;
  const UInt32 red = (redWeight * 255 + 24) / 48;
  const UInt32 green = (greenWeight * 255 + 24) / 48;
  const UInt32 blue = (blueWeight * 255 + 24) / 48;
  return 0xff000000U | (red << 16) | (green << 8) | blue;
}

static bool quadColorMatches(UInt32 observed, UInt32 expected) {
  if (expected == kClearExpected || expected == kSampleBackground)
    return observed == expected;
  if ((observed >> 24) != 0xff)
    return false;

  for (unsigned shift = 0; shift < 24; shift += 8) {
    const int delta = static_cast<int>((observed >> shift) & 0xff) -
                      static_cast<int>((expected >> shift) & 0xff);
    if (delta < -kChannelTolerance || delta > kChannelTolerance)
      return false;
  }
  return true;
}

static bool isTestPixel(UInt32 color) {
  if (color == kClearExpected || color == kSampleBackground || color == kClearSentinel)
    return true;
  const UInt32 sum = ((color >> 16) & 0xff) + ((color >> 8) & 0xff) + (color & 0xff);
  // The sum stays near 255 for any interpolation of the opaque primaries.
  return (color >> 24) == 0xff && sum >= 249 && sum <= 261;
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
  _renderTextureSubmitAttempts = kRenderTextureSubmitAttempts;
  _renderTextureScratch = NULL;
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

  // Let the marker run on its own refresh before submitting the two passes.
  if (_renderTextureSubmitAttempts != 0 && _markerSubmitAttempts == 0 && !markerWasPending) {
    if (!_markerScratch &&
        _ringBufferWritePtr <= kXenosRingbufferLength - kRenderTextureRingDwords &&
        readReg32(kXenosRegCpRbReadPtr) == _ringBufferWritePtr &&
        readReg32(kXenosRegCpRbWritePtr) == _ringBufferWritePtr) {
      _renderTextureSubmitAttempts = 0;
      runRenderTextureTest();
    } else if (--_renderTextureSubmitAttempts == 0) {
      IOLog("XenosRenderTexture: SKIP no drained ring available after marker test\n");
    }
  }

  // The restore is already queued. Keep the ring intact if the GPU stalled.
  if (_renderTextureScratch && readReg32(kXenosRegCpRbReadPtr) != _ringBufferWritePtr) {
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

void XenosController::runRenderTextureTest() {
  // A 1920x1080, single-sample colour surface uses at most 1632 tiles
  // starting at tile zero. The two test targets each reserve four disjoint tiles.
  if (!_fbWidth || !_fbHeight || _fbWidth > 1920 || _fbHeight > 1080) {
    IOLog("XenosRenderTexture: SKIP display layout outside the tested eDRAM range\n");
    return;
  }

  // Discard, texture, sampled result, fence, vertices and indices.
  const UInt32 scratchBytes = 6 * PAGE_SIZE;
  IOBufferMemoryDescriptor *scratch = IOBufferMemoryDescriptor::withOptions(
      kIOMemoryPhysicallyContiguous, scratchBytes, PAGE_SIZE);
  if (!scratch) {
    IOLog("XenosRenderTexture: SKIP scratch allocation failed\n");
    return;
  }

  IOByteCount length = 0;
  const IOPhysicalAddress physical = scratch->getPhysicalSegment(0, &length);
  UInt32 *memory = static_cast<UInt32 *>(scratch->getBytesNoCopy());
  if (!memory || !physical || length < scratchBytes || (physical & (PAGE_SIZE - 1)) ||
      physical > kDisplayMemoryStart - scratchBytes) {
    IOLog("XenosRenderTexture: SKIP scratch range is outside usable GPU RAM\n");
    scratch->release();
    return;
  }

  const UInt32 discard = static_cast<UInt32>(physical);
  const UInt32 texture = discard + PAGE_SIZE;
  const UInt32 readback = texture + PAGE_SIZE;
  const UInt32 fence = readback + PAGE_SIZE;
  const UInt32 vertexAddress = fence + PAGE_SIZE;
  const UInt32 indexAddress = vertexAddress + PAGE_SIZE;
  const UInt32 samplePositionAddress = vertexAddress + CACHELINE_SIZE;
  const UInt32 sampleUvAddress = vertexAddress + 2 * CACHELINE_SIZE;
  UInt8 *bytes = reinterpret_cast<UInt8 *>(memory);
  volatile UInt32 *texturePixels = reinterpret_cast<volatile UInt32 *>(bytes + PAGE_SIZE);
  volatile UInt32 *pixels = reinterpret_cast<volatile UInt32 *>(bytes + 2 * PAGE_SIZE);
  volatile UInt32 *marker = reinterpret_cast<volatile UInt32 *>(bytes + 3 * PAGE_SIZE);
  UInt8 *vertices = bytes + 4 * PAGE_SIZE;
  void *indices = bytes + 5 * PAGE_SIZE;
  memcpy(vertices, kIndexedQuadVertices, sizeof(kIndexedQuadVertices));
  memcpy(vertices + CACHELINE_SIZE, kSamplePositions, sizeof(kSamplePositions));
  memcpy(vertices + 2 * CACHELINE_SIZE, kSampleUVs, sizeof(kSampleUVs));
  memcpy(indices, kQuadIndices, sizeof(kQuadIndices));
  syncData(vertices, 2 * CACHELINE_SIZE + sizeof(kSampleUVs));
  syncData(indices, sizeof(kQuadIndices));
  for (UInt32 i = 0; i < kClearPixels; ++i) {
    texturePixels[i] = kClearSentinel;
    pixels[i] = kClearSentinel;
  }
  *marker = kClearSentinel;
  for (UInt32 offset = 0; offset < 2 * PAGE_SIZE; offset += CACHELINE_SIZE)
    flushMarkerCacheLine(texturePixels + offset / sizeof(UInt32));
  flushMarkerCacheLine(marker);

  const UInt32 savedClear = readReg32(4 * kXenosColorClear);
  const UInt32 savedClearLo = readReg32(4 * (kXenosColorClear + 1));
  const UInt32 savedCoherDest = readReg32(4 * kXenosCoherDestBase1);
  const UInt32 savedVertexMax = readReg32(4 * REG_A2XX_VGT_MAX_VTX_INDX);
  writeRingPacket0(kXenosWaitUntil, 1);
  writeRing(kWait3dIdleClean);
  setGpuViewport(kClearSide, kClearSide);
  setGpuScissors(kClearSide, kClearSide);
  clearTextureTestTarget(kClearTile, discard, kClearColor);
  syncGpuMemory(vertexAddress, 2 * PAGE_SIZE, kCoherVertexCache);
  writeRingPacket0(REG_A2XX_RB_COPY_CONTROL, 1);
  writeRing(0);
  setGpuVertices(vertexAddress, sizeof(kIndexedQuadVertices));
  setGpuColorTriangleShaders();
  setGpuEdRamMode(COLOR_DEPTH);
  writeRingPacket0(REG_A2XX_VGT_MAX_VTX_INDX, 1);
  writeRing(kQuadVertexCount - 1);
  // The five dwords after this point are the indexed packet header and
  // its four payload words: visibility, initiator, DMA base, DMA count.
  writeRingPacket3(CP_DRAW_INDX, 4);
  writeRing(0);
  writeRing((kQuadIndexCount << 16) | CP_DRAW_INDX_1_PRIM_TYPE(DI_PT_TRILIST) |
            CP_DRAW_INDX_1_SOURCE_SELECT(DI_SRC_SEL_DMA) |
            CP_DRAW_INDX_1_INDEX_SIZE(INDEX_SIZE_16_BIT));
  writeRing(indexAddress);
  writeRing(kQuadIndexCount | kIndexDmaSwap8In16);
  writeRingPacket0(kXenosWaitUntil, 1);
  writeRing(kWait3dIdleClean);
  writeRingPacket0(REG_A2XX_VGT_MAX_VTX_INDX, 1);
  writeRing(savedVertexMax);

  // Resolve the first target as tiled ARGB and flush it for the texture unit.
  syncGpuMemory(_triagPhys, PAGE_SIZE, kCoherVertexCache);
  setGpuDisplayVertices();
  writeRingPacket3(CP_INVALIDATE_STATE, 1);
  writeRing(0x00000300);
  setGpuDisplayShaders();
  resolveTextureTestTarget(texture);

  clearTextureTestTarget(kSampleTile, discard, kSampleClearColor);
  writeRingPacket0(REG_A2XX_RB_COPY_CONTROL, 1);
  writeRing(0);
  setGpuVertices(samplePositionAddress, sizeof(kSamplePositions),
                 sampleUvAddress, sizeof(kSampleUVs));
  setGpuTexture(texture, kClearSide, kClearSide, SQ_TEX_FILTER_POINT, true);
  writeRingPacket3(CP_INVALIDATE_STATE, 1);
  writeRing(0x00000200);
  setGpuTexturePixelShader(true);
  setGpuEdRamMode(COLOR_DEPTH);
  writeRingPacket3(CP_DRAW_INDX, 2);
  writeRing(0);
  writeRing((4 << 16) | DRAW(DI_PT_TRISTRIP, DI_SRC_SEL_AUTO_INDEX,
    INDEX_SIZE_IGN, IGNORE_VISIBILITY));
  writeRingPacket0(kXenosWaitUntil, 1);
  writeRing(kWait3dIdleClean);

  // Return the framebuffer bindings before resolving the second target.
  syncGpuMemory(_triagPhys, PAGE_SIZE, kCoherVertexCache);
  setGpuDisplayVertices();
  setGpuTexture(_fbPhysAddr, _fbWidth, _fbHeight, SQ_TEX_FILTER_BILINEAR, false);
  writeRingPacket3(CP_INVALIDATE_STATE, 1);
  writeRing(0x00000200);
  setGpuTexturePixelShader(false);
  resolveTextureTestTarget(readback);

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
  writeRing(kRenderTextureComplete);
  syncRingBuffer();

  UInt32 seen = kClearSentinel;
  for (UInt32 poll = 0; poll < kMarkerPollLimit; ++poll) {
    flushMarkerCacheLine(marker);
    seen = *marker;
    if (seen == kRenderTextureComplete)
      break;
    IODelay(kMarkerPollDelayUs);
  }
  if (seen != kRenderTextureComplete) {
    IOLog("XenosRenderTexture: FAIL fence=0x%08lx rptr=0x%04lx wptr=0x%04lx\n",
          seen, readReg32(kXenosRegCpRbReadPtr), readReg32(kXenosRegCpRbWritePtr));
    _renderTextureScratch = scratch;
    return;
  }

  for (UInt32 offset = 0; offset < 2 * PAGE_SIZE; offset += CACHELINE_SIZE)
    flushMarkerCacheLine(texturePixels + offset / sizeof(UInt32));

  // Reuse the discard page for each row-major IORegistry image after the fence.
  const char *const properties[] = { "XenosRenderTextureSourcePixels", "XenosRenderTextureQuadPixels" };
  volatile UInt32 *const images[] = { texturePixels, pixels };
  UInt32 passes = 0;
  UInt32 sampleMismatches = 0;
  for (UInt32 pass = 0; pass < 2; ++pass) {
    const bool sampled = pass == 1;
    UInt32 mismatches = 0, untouched = 0, unexpected = 0;
    UInt32 quadMatches = 0, backgroundMatches = 0, firstMismatch = kClearPixels;
    for (UInt32 y = 0; y < kClearSide; ++y) {
      for (UInt32 x = 0; x < kClearSide; ++x) {
        const UInt32 i = y * kClearSide + x;
        const UInt32 observed = images[pass][resolveTileWord(x, y)];
        const bool inside = insideQuad(x, y);
        const UInt32 expected = sampled
          ? (inside ? expectedQuadColor(kClearSide - 1 - x, y) : kSampleBackground)
          : expectedQuadColor(x, y);
        memory[i] = observed;
        untouched += observed == kClearSentinel;
        unexpected += !isTestPixel(observed);
        if (sampled && inside)
          sampleMismatches += observed != texturePixels[resolveTileWord(kClearSide - 1 - x, y)];
        if (quadColorMatches(observed, expected)) {
          quadMatches += inside;
          backgroundMatches += !inside;
        } else {
          if (mismatches == 0)
            firstMismatch = i;
          ++mismatches;
        }
      }
    }
    const bool published = unexpected == 0 &&
                           setProperty(properties[pass], memory, kClearBytes);
    IOLog("XenosRenderTexture: pass=%s quad=%lu background=%lu untouched=%lu "
          "unexpected=%lu mismatches=%lu ioreg=%s\n",
          sampled ? "sampled" : "source", quadMatches, backgroundMatches, untouched,
          unexpected, mismatches, published ? "yes" : "no");
    if (mismatches == 0) {
      ++passes;
    } else {
      IOLog("XenosRenderTexture: pass=%s first mismatch=(%lu,%lu) observed=0x%08lx\n",
            sampled ? "sampled" : "source", firstMismatch % kClearSide,
            firstMismatch / kClearSide, memory[firstMismatch]);
    }
  }
  IOLog("XenosRenderTexture: %s 32x32 passes=%lu/2 sample_mismatches=%lu\n",
        passes == 2 && sampleMismatches == 0 ? "PASS" : "FAIL", passes, sampleMismatches);
  scratch->release();
}

void XenosController::clearTextureTestTarget(UInt32 tile, UInt32 discard, UInt32 color) {
  writeRingPacket0(REG_A2XX_RB_SURFACE_INFO, 2);
  writeRing(kClearEdRamPitch);
  writeRing((k_8_8_8_8 << 16) | tile);
  setGpuEdRamMode(EDRAM_COPY);
  writeRingPacket0(kXenosColorClear, 2);
  writeRing(color);
  writeRing(color);
  setResolveTarget(discard, kClearSide, kClearSide,
                   kResolveConvert | kResolveColorClear, kResolveDisplayFormat | kMarkerSwap8In32);
  drawResolveRectangle();
  writeRingPacket0(kXenosWaitUntil, 1);
  writeRing(kWait3dIdleClean);
}

void XenosController::resolveTextureTestTarget(UInt32 address) {
  setGpuEdRamMode(EDRAM_COPY);
  setResolveTarget(address, kClearSide, kClearSide, kResolveConvert,
                   kResolveDisplayFormat | kMarkerSwap8In32);
  drawResolveRectangle();
  writeRingPacket3(CP_EVENT_WRITE, 1);
  writeRing(kCacheFlushEvent);
  writeRingPacket0(kXenosCoherDestBase1, 1);
  writeRing(0);
  syncGpuMemory(address, PAGE_SIZE, kCoherResolveActions);
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
