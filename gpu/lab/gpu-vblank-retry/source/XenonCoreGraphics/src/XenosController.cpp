//
//  XenosController.cpp
//  Xbox 360 graphics controller
//
//  Copyright © 2026 John Davis. All rights reserved.
//

#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <ppc/machine_routines.h>

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
static const IOPhysicalAddress kReservedScanoutStart = kDisplayMemoryStart - kXenosFramebufferLength;
static const char kReservedScanoutProperty[] = "xbox360-vblank-scanout";

static const UInt32 kMovingDelay = 1200; // About 18 seconds of normal display refreshes.
static const UInt32 kMovingFrames = 480;
static const UInt32 kRefreshUS = 15000;
static const UInt32 kFencePollUS = 1000;
static const UInt32 kMovingWaitLimit = 240; // Fence polls, approximately 240 ms.
static const UInt32 kVBlankWaitLimitUS = 250000;
static const UInt32 kVBlankPollUS = 120;
static const UInt32 kEarlyBlankLines = 16;
static const UInt32 kApplyPolls = 32;
static const UInt32 kMovingRingDwords = 256;
static const UInt32 kTextureSide = 32;
static const UInt32 kTextureScale = 4;
static const UInt32 kLayerSide = kTextureSide * kTextureScale;
static const UInt32 kTravel = 64;
static const UInt32 kPixelsPerFrame = 4;
static const UInt32 kScanoutTileSide = 32;
static const UInt32 kLayerYOffset = 32;
static const UInt16 kXenosWaitUntil = 0x05c8;
static const UInt32 kWait3dIdleClean = 1 << 17;
static const UInt16 kXenosCoherDestBase1 = 0x2007;
static const UInt16 kXenosCoherSizeHost = 0x0a2f;
static const UInt16 kXenosCoherStatusHost = 0x0a31;
static const UInt32 kXenosRegD1GrphPitch = 0x6120;
static const UInt32 kXenosRegD1GrphEndY = 0x6138;
static const UInt32 kXenosRegDisplayBase = 0x2840;
static const UInt32 kXenosRegCrtcHTotal = 0x6000;
static const UInt32 kXenosRegCrtcVTotal = 0x6010;
static const UInt32 kXenosRegCrtcUpdate = 0x60e8;
static const UInt32 kXenosRegCrtcApply = 0x60ec;
static const UInt32 kXenosRegGraphicsUpdate = 0x6144;
static const UInt32 kXenosRegScanline = 0x6530;
static const UInt32 kXenosRegScanStatus = 0x6534;
static const UInt32 kXenosRegScalerUpdate = 0x65cc;
static const UInt32 kVBlankActive = 1U << 12;
static const UInt32 kScanlineMask = 0x1fff;
static const UInt32 kCoherVertexCache = 1 << 24;
static const UInt32 kCoherVertexAndTextureCaches = kCoherVertexCache | (1 << 25);
static const UInt32 kCoherResolveActions = kCoherVertexAndTextureCaches | 0x00000100;
static const UInt32 kCoherBusy = 1U << 31;
static const UInt32 kCacheFlushEvent = 6;
static const int kBlendTolerance = 1;
static const UInt32 kLowAlpha = 64;
static const UInt32 kHighAlpha = 192;

// Bottom-left, bottom-right, top-left, top-right.
static const float kLayerUVs[] = {
  0.0f, 1.0f,  1.0f, 1.0f,
  0.0f, 0.0f,  1.0f, 0.0f,
};

struct LayerOrigins {
  UInt32 backX, backY;
  UInt32 frontX, frontY;
};

static LayerOrigins movingLayerOrigins(UInt32 frame, UInt32 width, UInt32 height) {
  const UInt32 step = (frame * kPixelsPerFrame) % (2 * kTravel);
  const UInt32 travel = step <= kTravel ? step : 2 * kTravel - step;
  const UInt32 left = (width - kLayerSide - kTravel) / 2;
  const UInt32 top = (height - kLayerSide - kLayerYOffset) / 2;
  const LayerOrigins origins = {
    left + travel, top, left + kTravel - travel, top + kLayerYOffset
  };
  return origins;
}

// Encode a signed clip-space ratio as binary32 using integer operations.
// The timer runs in kernel context, where it must not touch FPU state.
static UInt32 clipCoordinate(int numerator, UInt32 extent) {
  if (numerator == 0)
    return 0;
  const UInt32 sign = numerator < 0 ? 0x80000000U : 0;
  UInt32 value = numerator < 0 ? -numerator : numerator;
  UInt32 exponent = 127;
  while (value < extent) {
    value *= 2;
    --exponent;
  }
  value -= extent;
  UInt32 fraction = 0;
  for (unsigned i = 0; i < 23; ++i) {
    value *= 2;
    fraction <<= 1;
    if (value >= extent) {
      value -= extent;
      fraction |= 1;
    }
  }
  const bool roundUp = 2 * value > extent || (2 * value == extent && (fraction & 1));
  return sign | ((exponent << 23) + fraction + roundUp);
}

static void writeQuadPositions(UInt32 *vertices, UInt32 x, UInt32 y,
                               UInt32 width, UInt32 height) {
  const UInt32 left = clipCoordinate(2 * static_cast<int>(x) - static_cast<int>(width), width);
  const UInt32 right = clipCoordinate(2 * static_cast<int>(x + kLayerSide) - static_cast<int>(width), width);
  const UInt32 top = clipCoordinate(static_cast<int>(height) - 2 * static_cast<int>(y), height);
  const UInt32 bottom = clipCoordinate(static_cast<int>(height) - 2 * static_cast<int>(y + kLayerSide), height);
  const UInt32 positions[] = {left, bottom, 0, right, bottom, 0, left, top, 0, right, top, 0};
  memcpy(vertices, positions, sizeof(positions));
}

// The top strip is outside the motion and blend probes. Two opaque texels
// encode the GPU-rendered frame number and its complement per RGB channel.
static UInt32 frameColor(UInt32 frame) {
  return 0xff800000U | frame;
}

static UInt32 frameCheckColor(UInt32 frame) {
  return frameColor(frame) ^ 0x00ffffffU;
}

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

static UInt32 textureSourceOver(UInt32 source, UInt32 destination) {
  const UInt32 alpha = source >> 24;
  // The back texture has opaque alpha.
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
  _movingDelay = kMovingDelay;
  _movingWait = 0;
  _vblankWaitUS = 0;
  _frontIndex = 0;
  _bufferFlips[0] = _bufferFlips[1] = 0;
  _orderErrors = _lateFlips = _latchErrors = 0;
  _lastFlipLine = 0;
  _needActiveScan = _frameReady = _desktopRestored = _flipAttempted = false;
  _alternateMap = NULL;
  _alternatePhysical = 0;
  _movingSubmitted = _movingChecked = _movingChanges = 0;
  _movingOpaque = _movingLow = _movingHigh = _movingMismatches = 0;
  _movingFinished = false;
  _movingScratch = NULL;
  _movingScanout = NULL;
  setProperty("XenosVBlankFlipResult", "Pending");
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

  if (_movingDelay)
    --_movingDelay;
  UInt32 interval = kRefreshUS;
  // Keep the batch intact if the marker timed out. A stalled GPU may still
  // own its scratch page.
  if (!_markerScratch || readReg32(kXenosRegCpRbReadPtr) == _ringBufferWritePtr) {
    const UInt32 diagnosticInterval = _movingDelay ? 0 : stepMovingComposite();
    if (diagnosticInterval) {
      interval = diagnosticInterval;
    } else {
      setGpuEdRamMode(COLOR_DEPTH);
      drawQuad();
      setGpuEdRamMode(EDRAM_COPY);
      drawResolveRectangle();
      syncRingBuffer();
    }
  }
  sender->setTimeoutUS(interval);
}

void XenosController::drawQuad() {
  writeRingPacket3(CP_DRAW_INDX, 2);
  writeRing(0);
  writeRing((4 << 16) | DRAW(DI_PT_TRISTRIP, DI_SRC_SEL_AUTO_INDEX,
    INDEX_SIZE_IGN, IGNORE_VISIBILITY));
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
      physical > kReservedScanoutStart - PAGE_SIZE) {
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

bool XenosController::scanoutAt(UInt32 address) {
  return readReg32(kXenosRegD1GrphPriSurfaceAddr) == address &&
         readReg32(kXenosRegDisplayBase) == address;
}

bool XenosController::displayModeMatches() {
  return readReg32(kXenosRegD1GrphPitch) == _fbWidth &&
         readReg32(kXenosRegD1GrphEndY) == _vblankHeight &&
         (readReg32(kXenosRegCrtcHTotal) & kScanlineMask) == _vblankHTotal &&
         (readReg32(kXenosRegCrtcVTotal) & kScanlineMask) == _vblankVTotal;
}

UInt32 XenosController::scanoutAddress(UInt32 index) const {
  return index ? _alternatePhysical : _gpuPhysAddr;
}

volatile UInt32 *XenosController::scanoutBuffer(UInt32 index) {
  if (index)
    return reinterpret_cast<volatile UInt32 *>(_alternateMap->getVirtualAddress());
  return reinterpret_cast<volatile UInt32 *>(_movingScanout->getVirtualAddress());
}

bool XenosController::initMovingComposite() {
  const bool supportedSize = (_fbWidth == 1280 && _fbHeight == 720) ||
                             (_fbWidth == 1920 && _fbHeight == 1080);
  if (!supportedSize || _gpuPhysAddr != kDisplayMemoryStart + kXenosFramebufferLength) {
    finishMovingComposite("SKIP", "unsupported framebuffer layout");
    return false;
  }

  // Use only the exact bank carved out by the matching OpenBIOS build.
  // Earlier firmware can boot this kext but cannot lend it physical memory.
  UInt32 range[2] = {0, 0};
  IORegistryEntry *chosen = IORegistryEntry::fromPath("/chosen", gIODTPlane);
  if (chosen) {
    const OSData *property = OSDynamicCast(OSData, chosen->getProperty(kReservedScanoutProperty));
    if (property && property->getLength() == sizeof(range))
      memcpy(range, property->getBytesNoCopy(), sizeof(range));
    chosen->release();
  }
  const UInt32 capacity = _fbWidth * ALIGN(_fbHeight, 32) * sizeof(UInt32);
  if (range[0] != kReservedScanoutStart || range[1] != kXenosFramebufferLength ||
      capacity > range[1]) {
    finishMovingComposite("SKIP", "missing or invalid OpenBIOS scanout reservation");
    return false;
  }
  _vblankHeight = readReg32(kXenosRegD1GrphEndY);
  _vblankHTotal = readReg32(kXenosRegCrtcHTotal) & kScanlineMask;
  _vblankVTotal = readReg32(kXenosRegCrtcVTotal) & kScanlineMask;
  const bool native720 = _vblankHeight == 720 && _vblankHTotal == 1649 && _vblankVTotal == 749;
  const bool native1080 = _vblankHeight == 1080 && _vblankHTotal == 2199 && _vblankVTotal == 1124;
  if ((!native720 && !native1080) || !scanoutAt(_gpuPhysAddr) || !displayModeMatches()) {
    finishMovingComposite("SKIP", "unsupported scanout surface, pitch, or timing");
    return false;
  }
  IODeviceMemory *surface = IODeviceMemory::withRange(_gpuPhysAddr, kXenosFramebufferLength);
  if (surface) {
    // Match the platform framebuffer mapping's default cache mode.
    _movingScanout = surface->map(kIOMapReadOnly);
    surface->release();
  }

  surface = IODeviceMemory::withRange(range[0], range[1]);
  if (surface) {
    _alternateMap = surface->map(kIOMapReadOnly);
    surface->release();
  }
  if (!_movingScanout || !_alternateMap) {
    finishMovingComposite("SKIP", "could not map both scanout surfaces");
    return false;
  }
  _alternatePhysical = range[0];
  setProperty("XenosVBlankFlipReservedBase", range[0], 32);
  setProperty("XenosVBlankFlipReservedBytes", range[1], 32);

  const UInt32 bytes = 4 * PAGE_SIZE;
  IOBufferMemoryDescriptor *scratch = IOBufferMemoryDescriptor::withOptions(
      kIOMemoryPhysicallyContiguous, bytes, PAGE_SIZE);
  IOByteCount length = 0;
  const IOPhysicalAddress physical = scratch ? scratch->getPhysicalSegment(0, &length) : 0;
  UInt8 *memory = scratch ? static_cast<UInt8 *>(scratch->getBytesNoCopy()) : NULL;
  if (!memory || !physical || length < bytes ||
      (physical & (PAGE_SIZE - 1)) || physical > kReservedScanoutStart - bytes) {
    if (scratch)
      scratch->release();
    finishMovingComposite("SKIP", "GPU scratch allocation failed");
    return false;
  }
  _movingScratch = scratch;
  _movingPhysical = static_cast<UInt32>(physical);
  memcpy(memory + 2 * CACHELINE_SIZE, kLayerUVs, sizeof(kLayerUVs));
  UInt32 *back = reinterpret_cast<UInt32 *>(memory + PAGE_SIZE);
  UInt32 *front = reinterpret_cast<UInt32 *>(memory + 2 * PAGE_SIZE);
  for (UInt32 y = 0; y < kTextureSide; ++y) {
    for (UInt32 x = 0; x < kTextureSide; ++x) {
      const UInt32 i = resolveTileWord(x, y);
      back[i] = backTexel(x, y);
      front[i] = frontTexel(x, y);
    }
  }
  syncData(memory + 2 * CACHELINE_SIZE, sizeof(kLayerUVs));
  syncData(back, 2 * PAGE_SIZE);
  volatile UInt32 *fence = reinterpret_cast<volatile UInt32 *>(memory + 3 * PAGE_SIZE);
  *fence = 0;
  flushMarkerCacheLine(fence);
  IOLog("XenosVBlankFlip: START render=%lux%lu active_height=%lu frames=%lu buffers=0x%08lx,0x%08lx\n",
        _fbWidth, _fbHeight, _vblankHeight, kMovingFrames, _gpuPhysAddr, _alternatePhysical);
  setProperty("XenosVBlankFlipResult", "Running");
  return true;
}

UInt32 XenosController::nextVBlankPollUS() {
  const UInt32 line = readReg32(kXenosRegScanline) & kScanlineMask;
  const UInt32 height = readReg32(kXenosRegD1GrphEndY);
  if (line >= height)
    return 1000;
  // Wake close to blanking, then poll in short intervals on the work loop.
  // Use the shorter of the 720p and 1080p line periods for the estimate.
  const UInt32 remaining = height - line;
  if (remaining > 24) {
    const UInt32 delay = (remaining - 16) * 14;
    return delay < 4000 ? delay : 4000;
  }
  return kVBlankPollUS;
}

XenosController::FlipResult XenosController::flipDuringVBlank(UInt32 address,
                                                            UInt32 activeHeight) {
  if (!(readReg32(kXenosRegScanStatus) & kVBlankActive)) {
    _needActiveScan = false;
    return kFlipRetry;
  }
  if (_needActiveScan)
    return kFlipRetry;

  // Keep the flag check and address commit together on the same CPU.
  const boolean_t interrupts = ml_set_interrupts_enabled(false);
  const UInt32 before = readReg32(kXenosRegScanline) & kScanlineMask;
  if (!(readReg32(kXenosRegScanStatus) & kVBlankActive) ||
      before < activeHeight || before >= activeHeight + kEarlyBlankLines ||
      (readReg32(kXenosRegCrtcUpdate) & 1) || readReg32(kXenosRegCrtcApply) ||
      (readReg32(kXenosRegGraphicsUpdate) & 1) ||
      (readReg32(kXenosRegScalerUpdate) & 0x10000)) {
    ml_set_interrupts_enabled(interrupts);
    return kFlipRetry;
  }

  _flipAttempted = true;
  writeReg32(kXenosRegDisplayBase, address);
  OSSynchronizeIO();
  writeReg32(kXenosRegD1GrphPriSurfaceAddr, address);
  OSSynchronizeIO();
  writeReg32(kXenosRegCrtcApply, 1);
  OSSynchronizeIO();
  UInt32 pending = readReg32(kXenosRegCrtcApply);
  for (UInt32 attempt = 0; pending && attempt < kApplyPolls; ++attempt)
    pending = readReg32(kXenosRegCrtcApply);
  const bool acknowledged = !pending && scanoutAt(address);
  const UInt32 after = readReg32(kXenosRegScanline) & kScanlineMask;
  const bool inBlank = (readReg32(kXenosRegScanStatus) & kVBlankActive) &&
                       after >= before && after >= activeHeight;
  ml_set_interrupts_enabled(interrupts);
  _needActiveScan = true;
  _lastFlipLine = after;
  _lateFlips += !inBlank;
  _latchErrors += !acknowledged;
  if (!acknowledged)
    return kFlipReadbackFailed;
  return inBlank ? kFlipComplete : kFlipLate;
}

bool XenosController::restoreDesktopScanout(bool drained) {
  if (_desktopRestored)
    return true;
  if (!drained)
    return false;

  if (_flipAttempted && (!scanoutAt(_gpuPhysAddr) || readReg32(kXenosRegCrtcApply))) {
    // Only restore our own scanout. Leave changes by another display owner alone.
    const UInt32 primary = readReg32(kXenosRegD1GrphPriSurfaceAddr);
    const UInt32 base = readReg32(kXenosRegDisplayBase);
    if (!_alternateMap ||
        (primary != _gpuPhysAddr && primary != _alternatePhysical) ||
        (base != _gpuPhysAddr && base != _alternatePhysical))
      return false;
    const UInt32 height = readReg32(kXenosRegD1GrphEndY);
    const FlipResult restore = flipDuringVBlank(_gpuPhysAddr, height);
    if ((restore != kFlipComplete && restore != kFlipLate) || !scanoutAt(_gpuPhysAddr))
      return false;
    IOLog("XenosVBlankFlip: desktop scanout restored at line %lu%s\n",
          _lastFlipLine, restore == kFlipLate ? " (late)" : "");
  }

  if (_movingScanout) {
    _movingScanout->release();
    _movingScanout = NULL;
  }
  if (_alternateMap) {
    _alternateMap->release();
    _alternateMap = NULL;
  }
  // The reserved scanout remains owned by the firmware. Keep scratch wired
  // too if an unfinished GPU batch might still use it.
  if (_movingScratch && (_movingSubmitted == _movingChecked || _frameReady)) {
    _movingScratch->release();
    _movingScratch = NULL;
  }
  _desktopRestored = true;
  setProperty("XenosVBlankFlipDesktopRestored", true);
  return true;
}

UInt32 XenosController::stepMovingComposite() {
  const bool drained = readReg32(kXenosRegCpRbReadPtr) == _ringBufferWritePtr &&
                       readReg32(kXenosRegCpRbWritePtr) == _ringBufferWritePtr;
  if (_movingFinished)
    return restoreDesktopScanout(drained) ? 0 : nextVBlankPollUS();

  if (!_movingScratch) {
    if (!drained)
      return kFencePollUS;
    if (!initMovingComposite())
      return restoreDesktopScanout(drained) ? 0 : kRefreshUS;
  }
  if (!displayModeMatches() || !scanoutAt(scanoutAddress(_frontIndex))) {
    finishMovingComposite("FAIL", "display changed during the test");
    return kFencePollUS;
  }

  const UInt32 backIndex = 1 - _frontIndex;
  if (_movingSubmitted == _movingChecked) {
    if (!drained)
      return kFencePollUS;
    if (_ringBufferWritePtr > kXenosRingbufferLength - kMovingRingDwords) {
      // Keep each batch contiguous. Type-2 NOPs take us to ring slot zero.
      while (_ringBufferWritePtr)
        writeRing(2U << 30);
      syncRingBuffer();
      return kFencePollUS;
    }
    _movingWait = _vblankWaitUS = 0;
    _frameReady = false;
    submitMovingComposite(scanoutAddress(backIndex));
    return kFencePollUS;
  }

  if (!_frameReady) {
    UInt8 *memory = static_cast<UInt8 *>(_movingScratch->getBytesNoCopy());
    volatile UInt32 *fence = reinterpret_cast<volatile UInt32 *>(memory + 3 * PAGE_SIZE);
    if (drained) {
      flushMarkerCacheLine(fence);
      _frameReady = *fence == _movingSubmitted;
    }
    if (!_frameReady) {
      if (++_movingWait >= kMovingWaitLimit)
        finishMovingComposite("FAIL", drained ? "GPU completion marker missing" : "GPU ring did not drain");
      return kFencePollUS;
    }
    // The active surface must still contain the previous frame after rendering
    // into the other surface. Its serial is independent of position and alpha.
    if (_movingChecked && !checkFrameNumber(_frontIndex, _movingChecked)) {
      finishMovingComposite("FAIL", "front buffer changed before flip");
      return kFencePollUS;
    }
  }

  const FlipResult flip = flipDuringVBlank(scanoutAddress(backIndex), _vblankHeight);
  if (flip == kFlipRetry) {
    const UInt32 delay = nextVBlankPollUS();
    _vblankWaitUS += delay;
    if (_vblankWaitUS > kVBlankWaitLimitUS)
      finishMovingComposite("FAIL", "timed out waiting for VBlank");
    return delay;
  }
  if (flip != kFlipComplete) {
    finishMovingComposite("FAIL", flip == kFlipLate ? "flip crossed out of VBlank" : "scanout address did not latch");
    return kFencePollUS;
  }

  _frontIndex = backIndex;
  ++_bufferFlips[_frontIndex];
  if (!checkFrameNumber(_frontIndex, _movingSubmitted)) {
    finishMovingComposite("FAIL", "displayed frame number out of order");
    return kFencePollUS;
  }
  checkMovingComposite(scanoutBuffer(_frontIndex), _movingChecked);
  ++_movingChecked;
  if (_movingChecked == kMovingFrames) {
    const bool passed = !_movingMismatches && !_orderErrors && !_lateFlips && _movingOpaque &&
                        _movingLow && _movingHigh && _movingChanges == kMovingFrames - 1 &&
                        _bufferFlips[0] == kMovingFrames / 2 && _bufferFlips[1] == kMovingFrames / 2;
    finishMovingComposite(passed ? "PASS" : "FAIL", "frame checks complete");
    return restoreDesktopScanout(true) ? 0 : kFencePollUS;
  }
  return kFencePollUS;
}

void XenosController::submitMovingComposite(UInt32 target) {
  UInt8 *memory = static_cast<UInt8 *>(_movingScratch->getBytesNoCopy());
  const LayerOrigins origins = movingLayerOrigins(_movingSubmitted, _fbWidth, _fbHeight);
  writeQuadPositions(reinterpret_cast<UInt32 *>(memory), origins.backX, origins.backY,
                     _fbWidth, _fbHeight);
  writeQuadPositions(reinterpret_cast<UInt32 *>(memory + CACHELINE_SIZE),
                     origins.frontX, origins.frontY, _fbWidth, _fbHeight);
  syncData(memory, 2 * CACHELINE_SIZE);

  UInt32 *back = reinterpret_cast<UInt32 *>(memory + PAGE_SIZE);
  back[resolveTileWord(0, 0)] = frameColor(_movingSubmitted + 1);
  back[resolveTileWord(2, 0)] = frameCheckColor(_movingSubmitted + 1);
  syncData(back, CACHELINE_SIZE);

  const UInt32 savedCoherDest = readReg32(4 * kXenosCoherDestBase1);
  writeRingReg32(4 * kXenosWaitUntil, kWait3dIdleClean);
  setGpuEdRamMode(COLOR_DEPTH);
  drawQuad(); // Draw the current desktop using the regular display state.
  writeRingReg32(4 * kXenosWaitUntil, kWait3dIdleClean);

  if (_movingSubmitted == 0)
    syncGpuMemory(_movingPhysical, 3 * PAGE_SIZE, kCoherVertexAndTextureCaches);
  else
    syncGpuMemory(_movingPhysical, 2 * PAGE_SIZE, kCoherVertexAndTextureCaches);
  writeRingPacket3(CP_INVALIDATE_STATE, 1);
  writeRing(0x00000200);
  setGpuTexturePixelShader(true);
  setBlendTestState(false);
  const UInt32 uv = _movingPhysical + 2 * CACHELINE_SIZE;
  drawTextureLayer(_movingPhysical, uv, _movingPhysical + PAGE_SIZE);
  setBlendTestState(true);
  drawTextureLayer(_movingPhysical + CACHELINE_SIZE, uv, _movingPhysical + 2 * PAGE_SIZE);

  // Restore the regular refresh state before resolving into the hidden surface.
  writeRingPacket0(REG_A2XX_RB_BLEND_CONTROL, 2);
  writeRing(A2XX_RB_BLEND_CONTROL_COLOR_SRCBLEND(FACTOR_ONE) |
            A2XX_RB_BLEND_CONTROL_ALPHA_SRCBLEND(FACTOR_ONE));
  writeRing(A2XX_RB_COLORCONTROL_BLEND_DISABLE);
  syncGpuMemory(_triagPhys, PAGE_SIZE, kCoherVertexCache);
  setGpuDisplayVertices();
  setGpuTexture(_fbPhysAddr, _fbWidth, _fbHeight, SQ_TEX_FILTER_BILINEAR, false);
  writeRingPacket3(CP_INVALIDATE_STATE, 1);
  writeRing(0x00000200);
  setGpuTexturePixelShader(false);
  setGpuEdRamMode(EDRAM_COPY);
  setResolveTarget(target, _fbWidth, _fbHeight, kResolveConvert, kResolveDisplayFormat);
  drawResolveRectangle();
  writeRingReg32(4 * kXenosWaitUntil, kWait3dIdleClean);

  writeRingPacket3(CP_EVENT_WRITE, 1);
  writeRing(kCacheFlushEvent);
  writeRingReg32(4 * kXenosCoherDestBase1, 0);
  syncGpuMemory(target, _fbWidth * ALIGN(_fbHeight, 32) * sizeof(UInt32), kCoherResolveActions);
  writeRingReg32(4 * kXenosWaitUntil, kWait3dIdleClean);
  writeRingReg32(4 * kXenosCoherDestBase1, savedCoherDest);
  // Normal refresh can resume on buffer A without inheriting the hidden target.
  setResolveTarget(_gpuPhysAddr, _fbWidth, _fbHeight, kResolveConvert, kResolveDisplayFormat);
  writeRingPacket3(CP_MEM_WRITE, 2);
  writeRing((_movingPhysical + 3 * PAGE_SIZE) | kMarkerSwap8In32);
  writeRing(++_movingSubmitted);
  syncRingBuffer();
}

static UInt32 readScanoutPixel(volatile UInt32 *surface, UInt32 pitch, UInt32 x, UInt32 y) {
  const UInt32 tile = (y / kScanoutTileSide) * (pitch / kScanoutTileSide) + x / kScanoutTileSide;
  const UInt32 withinTile = resolveTileWord(x % kScanoutTileSide, y % kScanoutTileSide);
  volatile UInt32 *pixel = surface + tile * kScanoutTileSide * kScanoutTileSide + withinTile;
  flushMarkerCacheLine(pixel);
  return OSReadLittleInt32(pixel, 0);
}

bool XenosController::checkFrameNumber(UInt32 index, UInt32 frame) {
  const LayerOrigins origins = movingLayerOrigins(frame - 1, _fbWidth, _fbHeight);
  volatile UInt32 *surface = scanoutBuffer(index);
  const UInt32 word = readScanoutPixel(surface, _fbWidth, origins.backX + 1, origins.backY + 1);
  const UInt32 check = readScanoutPixel(surface, _fbWidth, origins.backX + 9, origins.backY + 1);
  if (word == frameColor(frame) && check == frameCheckColor(frame))
    return true;
  ++_orderErrors;
  IOLog("XenosVBlankFlip: serial mismatch buffer=%lu expected=%lu word=0x%08lx check=0x%08lx\n",
        index, frame, word, check);
  return false;
}

void XenosController::checkMovingComposite(volatile UInt32 *scanout, UInt32 frame) {
  const LayerOrigins origins = movingLayerOrigins(frame, _fbWidth, _fbHeight);
  UInt32 frameProbes = 0;
  UInt32 frameMismatches = 0;
  for (UInt32 dy = 7; dy < kLayerSide; dy += 16) {
    // A 6-pixel inset keeps the checkerboard probe away from texel-cell
    // transitions for every phase of the two strips.
    for (UInt32 dx = 6; dx < kLayerSide; dx += 16) {
      const UInt32 x = origins.backX + dx;
      const UInt32 y = origins.backY + dy;
      const UInt32 frontX = x - origins.frontX;
      const UInt32 frontY = y - origins.frontY;
      const bool overlap = frontX < kLayerSide && frontY < kLayerSide;
      // Avoid probes adjacent to an overlay's moving texel-cell boundaries.
      if (frontY < kLayerSide &&
          (frontX == static_cast<UInt32>(-1) || frontX == kLayerSide ||
           (overlap && ((frontX & 15) == 0 || (frontX & 15) == 15))))
        continue;

      UInt32 expected = backTexel(dx / kTextureScale, dy / kTextureScale);
      if (overlap) {
        const UInt32 source = frontTexel(frontX / kTextureScale, frontY / kTextureScale);
        expected = textureSourceOver(source, expected);
        if ((source >> 24) == kLowAlpha)
          ++_movingLow;
        else
          ++_movingHigh;
      } else {
        ++_movingOpaque;
      }

      const UInt32 observed = readScanoutPixel(scanout, _fbWidth, x, y);
      const bool match = compositePixelMatches(observed, expected, overlap);
      if (!match && _movingMismatches == 0 && frameMismatches == 0) {
        IOLog("XenosVBlankFlip: first mismatch frame=%lu pixel=(%lu,%lu) expected=0x%08lx observed=0x%08lx\n",
              frame + 1, x, y, expected, observed);
      }
      frameMismatches += !match;
      ++frameProbes;
    }
  }

  if (frame) {
    // Pick a probe on the exposed top edge where one texel cell moves over
    // the next. A frame left over from the previous submission fails here.
    const LayerOrigins previous = movingLayerOrigins(frame - 1, _fbWidth, _fbHeight);
    const UInt32 dx = origins.backX > previous.backX ? 14 : 18;
    const UInt32 x = origins.backX + dx;
    const UInt32 y = origins.backY + 7;
    const UInt32 expected = backTexel(dx / kTextureScale, 7 / kTextureScale);
    const UInt32 stale = backTexel((x - previous.backX) / kTextureScale, 7 / kTextureScale);
    const UInt32 observed = readScanoutPixel(scanout, _fbWidth, x, y);
    const bool moved = observed == expected && observed != stale;
    if (!moved && !_movingMismatches && !frameMismatches)
      IOLog("XenosVBlankFlip: motion mismatch frame=%lu pixel=(%lu,%lu) expected=0x%08lx observed=0x%08lx\n",
            frame + 1, x, y, expected, observed);
    _movingChanges += moved;
    frameMismatches += !moved;
    ++_movingOpaque;
    ++frameProbes;
  }
  _movingMismatches += frameMismatches;
  if (frame < 4 || (frame + 1) % 120 == 0) {
    IOLog("XenosVBlankFlip: frame=%lu buffer=%lu vblank_line=%lu probes=%lu mismatches=%lu\n",
          frame + 1, _frontIndex, _lastFlipLine, frameProbes, _movingMismatches);
  }
}

void XenosController::finishMovingComposite(const char *result, const char *reason) {
  _movingFinished = true;
  setProperty("XenosVBlankFlipFrames", _movingChecked, 32);
  setProperty("XenosVBlankFlipBufferA", _bufferFlips[0], 32);
  setProperty("XenosVBlankFlipBufferB", _bufferFlips[1], 32);
  setProperty("XenosVBlankFlipMovedFrames", _movingChanges, 32);
  setProperty("XenosVBlankFlipLowAlphaProbes", _movingLow, 32);
  setProperty("XenosVBlankFlipHighAlphaProbes", _movingHigh, 32);
  setProperty("XenosVBlankFlipPixelMismatches", _movingMismatches, 32);
  setProperty("XenosVBlankFlipOrderErrors", _orderErrors, 32);
  setProperty("XenosVBlankFlipLate", _lateFlips, 32);
  setProperty("XenosVBlankFlipLatchErrors", _latchErrors, 32);
  setProperty("XenosVBlankFlipResult", result);
  IOLog("XenosVBlankFlip: %s %s checked=%lu/%lu A=%lu B=%lu order_errors=%lu late=%lu latch_errors=%lu "
        "moved=%lu opaque=%lu alpha64=%lu alpha192=%lu mismatches=%lu\n",
        result, reason, _movingChecked, kMovingFrames, _bufferFlips[0], _bufferFlips[1],
        _orderErrors, _lateFlips, _latchErrors, _movingChanges, _movingOpaque, _movingLow, _movingHigh,
        _movingMismatches);
}

void XenosController::drawTextureLayer(UInt32 positions, UInt32 uv, UInt32 texture) {
  setGpuVertices(positions, 12 * sizeof(UInt32), uv, sizeof(kLayerUVs));
  setGpuTexture(texture, kTextureSide, kTextureSide, SQ_TEX_FILTER_POINT, true);
  drawQuad();
  writeRingReg32(4 * kXenosWaitUntil, kWait3dIdleClean);
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
