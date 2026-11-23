//
//  XenosController.cpp
//  Xbox 360 graphics controller
//
//  Copyright © 2026 John Davis. All rights reserved.
//

#include <IOKit/IOPlatformExpert.h>
#include <sys/random.h>

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
static const UInt32 kQuadRingDwords = 352;
static const UInt32 kQuadRenderTextureRingDwords = 512;
static const UInt32 kQuadMipmapRingDwords = 704;
static const UInt32 kQuadPagesBeforeTextures = 4;
static const UInt32 kQuadGeometryStride = 2 * CACHELINE_SIZE;
static const UInt32 kQuadReadbackOffset = PAGE_SIZE;
static const UInt32 kQuadFenceOffset = 2 * PAGE_SIZE;
static const UInt32 kSurfaceFenceOffset = PAGE_SIZE;
static const UInt32 kSurfaceStorageBytes = kSurfaceFenceOffset + PAGE_SIZE;
static const UInt32 kSurfaceAllocationLimit = 8;
static const UInt32 kQuadGeometryOffset = 3 * PAGE_SIZE;
static const UInt32 kQuadTextureOffset = 4 * PAGE_SIZE;
static const UInt32 kQuadSampleOffset = (kQuadPagesBeforeTextures + kXenosQuadBatchLayers) * PAGE_SIZE;
static const UInt32 kQuadMipOffset = kQuadSampleOffset + PAGE_SIZE;
static const UInt32 kQuadAnisoTextureOffset = kQuadMipOffset + PAGE_SIZE;
static const UInt32 kClearSide = kXenosQuadTargetSide;
static const UInt32 kClearEdRamPitch = 160; // Two 80-pixel eDRAM tiles per row.
static const UInt32 kClearPixels = kXenosQuadReadbackPixels;
static const UInt32 kClearTile = 0x7f0;
static const UInt32 kStencilTile = 0x7f8; // Separate eDRAM tiles, format 0 is D24S8.
static const UInt32 kSampleTile = 0x7f4; // Between the first color and depth/stencil allocations.
static const UInt32 kSampleBorder = 1;
static const UInt32 kMipSide = kClearSide / 2;
static const UInt32 kMipTailX = 16;
static const UInt32 kAnisoRegionCount = 2;
static const UInt32 kAnisoLeft[] = {2, 18};
static const UInt32 kAnisoTop = 5;
static const UInt32 kAnisoWidth = 12;
static const UInt32 kAnisoHeight = 6;
static const UInt32 kAnisoSlopeDenominator = 4;
static const UInt32 kAnisoU = 8;
static const UInt32 kAnisoVSpan = 8 * kAnisoHeight;
static const UInt32 kAniso16To1 = 5; // Fetch constant encoding.
static const UInt32 kAnisoColorTag = 0xff0060a0;
static const UInt32 kAnisoDarkRed = 16;
static const UInt32 kAnisoLightRed = 240;
static const UInt32 kAnisoGrayRed = (kAnisoDarkRed + kAnisoLightRed) / 2;
static const UInt32 kAnisoColorTolerance = 2;
static const UInt32 kAnisoMinStripeDelta = 32;
static const UInt32 kTrilinearRegionCount = 3;
static const UInt32 kTrilinearRegionX[] = {1, 11, 21};
static const UInt32 kTrilinearRegionY = 1;
static const UInt32 kTrilinearWidth = 8;
static const UInt32 kTrilinearHeight = 16;
static const UInt32 kTrilinearU = 5;
static const UInt32 kTrilinearV = 6;
static const UInt32 kTrilinearUQuarter = 1;
static const UInt32 kTrilinearVQuarter = 3;
static const UInt32 kTrilinearLodSteps[] = {0, 16, 32};
// Account for fixed-point interpolation and framebuffer quantization.
static const UInt32 kTrilinearTolerance = 5;
static const UInt32 kMipSampleOrigin = 1;
static const UInt32 kBaseSampleOrigin = 19;
static const UInt32 kBaseSampleSide = 12;
static const UInt32 kBaseSampleU = 3;
static const UInt32 kBaseSampleV = 4;
static const UInt32 kSampleOffsetUQuarter = 1;
static const UInt32 kSampleOffsetVQuarter = 3;
static const UInt32 kSampleClearColor = 0xff473421;
static const UInt32 kSampleBackground = 0xff213447;
static const UInt32 kStencilClear = 0xffffff00; // Depth 1.0, stencil 0.
static const UInt32 kStencilClearControl = 0x8777; // Resolve clear state shared with the depth test.
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
static const UInt16 kXenosDepthClear = 0x231d;
static const UInt16 kXenosHiZControl = 0x2203;
static const UInt32 kResolveColorClear = 1 << 8;
static const UInt32 kResolveDepthClear = 1 << 9;
static const UInt32 kCacheFlushEvent = 6;
static const UInt32 kQuadSide = kXenosQuadUploadSide;
static const UInt32 kQuadOrigin = 4;

// All vertex coordinates are small integer fractions with a power-of-two
// denominator. Encode exact IEEE-754 words without using the kernel's FPU.
static UInt32 coordinateWord(int value, unsigned fractionalBits) {
  if (!value)
    return 0;
  const UInt32 magnitude = value < 0 ? -value : value;
  const unsigned highBit = 31 - __builtin_clz(static_cast<unsigned>(magnitude));
  const UInt32 exponent = (127 + highBit - fractionalBits) << 23;
  const UInt32 mantissa = (magnitude << (23 - highBit)) & 0x007fffffU;
  return (value < 0 ? 0x80000000U : 0) | exponent | mantissa;
}

static void packQuadGeometry(const XenosQuadGeometry &geometry, UInt32 *positions,
                             UInt32 *uvs, bool rotated) {
  const int half = rotated ? 64 : 16;
  const unsigned fractionalBits = rotated ? 6 : 4;
  for (UInt32 index = 0; index < 4; ++index) {
    const XenosQuadVertex &vertex = geometry.vertices[index];
    // Positions use clip space. The sampler uses a padded 32x32 texture.
    positions[3 * index] = coordinateWord(int(vertex.x) - half, fractionalBits);
    positions[3 * index + 1] = coordinateWord(half - int(vertex.y), fractionalBits);
    positions[3 * index + 2] = 0;
    uvs[2 * index] = coordinateWord(vertex.u, 5);
    uvs[2 * index + 1] = coordinateWord(vertex.v, 5);
  }
}

static void packQuadSampleQuad(UInt8 *slot, const XenosQuadGeometry &geometry,
                                  int uOffsetQuarter, int vOffsetQuarter) {
  UInt32 *uv = reinterpret_cast<UInt32 *>(slot + CACHELINE_SIZE);
  packQuadGeometry(geometry, reinterpret_cast<UInt32 *>(slot), uv, false);
  // Encode quarter-texel offsets, normalized by the 32x32 base size.
  for (UInt32 index = 0; index < 4; ++index) {
    const XenosQuadVertex &vertex = geometry.vertices[index];
    uv[2 * index] = coordinateWord(4 * int(vertex.u) + uOffsetQuarter, 7);
    uv[2 * index + 1] = coordinateWord(4 * int(vertex.v) + vOffsetQuarter, 7);
  }
}

static void packQuadDepth(const XenosQuadGeometry &geometry, UInt32 *positions,
                           bool rotated, bool opaque, UInt32 splitX) {
  // The opaque surface ramps with screen X while the translucent quad lies
  // at Z=(splitX+8)/64. Pixel centers never sit exactly on the split.
  const UInt32 positionScale = rotated ? 1 : kXenosQuadQuarter;
  for (UInt32 index = 0; index < 4; ++index) {
    const UInt32 xQuarter = opaque ? geometry.vertices[index].x * positionScale
                                   : splitX * kXenosQuadQuarter;
    positions[3 * index + 2] = coordinateWord(xQuarter + 8 * kXenosQuadQuarter, 8);
  }
}

static UInt32 builtInTexel(UInt32 texture, UInt32 x, UInt32 y) {
  const UInt32 column = (x / 4) & 3;
  const UInt32 row = (y / 4) & 3;
  const UInt32 red = 176 + 12 * column;
  const UInt32 green = 32 + 20 * row;
  const UInt32 blue = 24 + 8 * (column ^ row);
  return 0xff000000U | (green << 8) |
         (texture == 0 ? (red << 16) | blue : (blue << 16) | red);
}

static UInt32 quadSourceOver(UInt32 source, UInt32 destination) {
  // The clear color and the first texture both have opaque alpha.
  const UInt32 alpha = source >> 24;
  UInt32 result = 0xff000000U;
  for (unsigned shift = 0; shift < 24; shift += 8) {
    const UInt32 src = (source >> shift) & 0xff;
    const UInt32 dst = (destination >> shift) & 0xff;
    result |= ((alpha * src + (255 - alpha) * dst + 127) / 255) << shift;
  }
  return result;
}

static UInt32 quadMaxChannelDelta(UInt32 observed, UInt32 expected) {
  UInt32 maximum = 0;
  for (unsigned shift = 0; shift < 32; shift += 8) {
    const UInt32 a = (observed >> shift) & 0xff;
    const UInt32 b = (expected >> shift) & 0xff;
    const UInt32 delta = a > b ? a - b : b - a;
    if (delta > maximum)
      maximum = delta;
  }
  return maximum;
}

static UInt32 quadHashColor(UInt32 hash, UInt32 color) {
  hash = xenosQuadHashByte(hash, (color >> 16) & 0xff);
  hash = xenosQuadHashByte(hash, (color >> 8) & 0xff);
  return xenosQuadHashByte(hash, color & 0xff);
}

static UInt32 quadBilinearColor(const UInt32 *samples, UInt32 u, UInt32 v) {
  const UInt32 scale = 64;
  const UInt32 weights[] = {(scale - u) * (scale - v), u * (scale - v), (scale - u) * v, u * v};
  const UInt32 totalWeight = scale * scale;
  UInt32 color = 0;
  for (UInt32 shift = 0; shift < 32; shift += 8) {
    UInt32 sum = 0;
    for (UInt32 index = 0; index < 4; ++index)
      sum += ((samples[index] >> shift) & 0xff) * weights[index];
    color |= ((sum + totalWeight / 2) / totalWeight) << shift;
  }
  return color;
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
  _quadUploaded = false;
  _quadGeometryMode = 0;
  _quadLayerCount = 0;
  _quadTicket = _nextQuadTicket = _quadTexture = _quadWait = _quadCooldown = 0;
  _gpuCompletionSequence = 0;
  _surfaceQueueCount = _surfaceActiveTicket = _surfaceCompletion = 0;
  bzero(_surfaceRecords, sizeof(_surfaceRecords));
  _surfaceSerial = _surfaceCount = _surfaceColor = _drawableReaders = 0;
  _surfaceJobPixels = _drawableGrantPixels = NULL;
  bzero(&_drawableGrantToken, sizeof(_drawableGrantToken));
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

static void prepareQuadReadback(UInt8 *address) {
  volatile UInt32 *pixels = reinterpret_cast<volatile UInt32 *>(address);
  for (UInt32 i = 0; i < kClearPixels; ++i)
    pixels[i] = kClearSentinel;
  for (UInt32 offset = 0; offset < PAGE_SIZE; offset += CACHELINE_SIZE)
    flushMarkerCacheLine(pixels + offset / sizeof(UInt32));
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

IOReturn XenosController::queueQuad(OSObject *owner, void *clientArg, void *rgb,
                                      void *output, void *textureArg) {
  XenosController *self = static_cast<XenosController *>(owner);
  const UInt32 texture = static_cast<UInt32>(reinterpret_cast<uintptr_t>(textureArg));
  return self->enqueueQuad(static_cast<XenosQuadClient *>(clientArg),
                           static_cast<const UInt8 *>(rgb), texture, NULL, 0,
                           static_cast<UInt32 *>(output));
}

IOReturn XenosController::queueQuadGeometry(OSObject *owner, void *clientArg, void *input,
                                             void *output, void *) {
  XenosController *self = static_cast<XenosController *>(owner);
  const XenosQuadGeometryUpload *upload = static_cast<const XenosQuadGeometryUpload *>(input);
  return self->enqueueQuad(static_cast<XenosQuadClient *>(clientArg), upload->rgb, 0,
                           &upload->geometry, kXenosQuadUploadGeometry, static_cast<UInt32 *>(output));
}

IOReturn XenosController::queueQuadCapture(OSObject *owner, void *clientArg, void *input,
                                             void *output, void *unused) {
  XenosQuadClient *client = static_cast<XenosQuadClient *>(clientArg);
  if (!client->_quadCapturePixels) {
    client->_quadCapturePixels = static_cast<UInt32 *>(
      IOMalloc(kXenosQuadReadbackPixels * sizeof(UInt32)));
    if (!client->_quadCapturePixels)
      return kIOReturnNoMemory;
  }
  const IOReturn result = queueQuadGeometry(owner, client, input, output, unused);
  if (result == kIOReturnSuccess)
    client->_captureQuad = true;
  return result;
}

IOReturn XenosController::readQuadCapture(OSObject *owner, void *clientArg, void *input,
                                            void *output, void *) {
  XenosQuadClient *client = static_cast<XenosQuadClient *>(clientArg);
  const XenosQuadReadRequest &request = *static_cast<const XenosQuadReadRequest *>(input);
  if (static_cast<XenosController *>(owner)->isInactive() || client->isInactive())
    return kIOReturnNotReady;
  if (!client->_captureQuad || !request.ticket || request.ticket != client->_ticket ||
      request.firstRow > kXenosQuadTargetSide - kXenosSurfaceReadRows ||
      request.firstRow % kXenosSurfaceReadRows)
    return kIOReturnBadArgument;
  if (client->_result[1] != request.ticket ||
      (client->_result[0] != kXenosQuadPassed && client->_result[0] != kXenosQuadPixelsFailed))
    return kIOReturnNotReady;
  memcpy(output, client->_quadCapturePixels + request.firstRow * kXenosQuadTargetSide,
         sizeof(XenosSurfaceReadback));
  return kIOReturnSuccess;
}

IOReturn XenosController::queueQuadRotated(OSObject *owner, void *clientArg, void *input,
                                            void *output, void *) {
  XenosController *self = static_cast<XenosController *>(owner);
  const XenosQuadGeometryUpload *upload = static_cast<const XenosQuadGeometryUpload *>(input);
  return self->enqueueQuad(static_cast<XenosQuadClient *>(clientArg), upload->rgb, 0,
                           &upload->geometry, kXenosQuadUploadRotated, static_cast<UInt32 *>(output));
}

IOReturn XenosController::queueQuadBatch(OSObject *owner, void *clientArg, void *input,
                                          void *output, void *) {
  XenosController *self = static_cast<XenosController *>(owner);
  const XenosQuadBatchUpload *batch = static_cast<const XenosQuadBatchUpload *>(input);
  return self->enqueueQuadBatch(static_cast<XenosQuadClient *>(clientArg), *batch, NULL,
                                kXenosQuadUploadBatch, NULL, NULL,
                                static_cast<UInt32 *>(output));
}

IOReturn XenosController::queueQuadBlendedBatch(OSObject *owner, void *clientArg, void *input,
                                                 void *output, void *) {
  XenosController *self = static_cast<XenosController *>(owner);
  const XenosQuadBlendedBatchUpload *upload = static_cast<const XenosQuadBlendedBatchUpload *>(input);
  return self->enqueueQuadBatch(static_cast<XenosQuadClient *>(clientArg), upload->batch,
                                upload->frontAlpha, kXenosQuadUploadBlendedBatch, NULL, NULL,
                                static_cast<UInt32 *>(output));
}

IOReturn XenosController::queueQuadScissoredBatch(OSObject *owner, void *clientArg, void *input,
                                                   void *output, void *) {
  XenosController *self = static_cast<XenosController *>(owner);
  const XenosQuadScissoredBatchUpload *batch = static_cast<const XenosQuadScissoredBatchUpload *>(input);
  return self->enqueueQuadBatch(static_cast<XenosQuadClient *>(clientArg), batch->upload.batch,
                                batch->upload.frontAlpha, kXenosQuadUploadScissoredBatch, NULL,
                                &batch->frontScissor,
                                static_cast<UInt32 *>(output));
}

IOReturn XenosController::queueQuadStencilBatch(OSObject *owner, void *clientArg, void *input,
                                                 void *output, void *) {
  XenosController *self = static_cast<XenosController *>(owner);
  const XenosQuadStencilBatchUpload *batch = static_cast<const XenosQuadStencilBatchUpload *>(input);
  return self->enqueueQuadBatch(static_cast<XenosQuadClient *>(clientArg), batch->upload.batch,
                                batch->upload.frontAlpha, kXenosQuadUploadStencilBatch, batch->mask,
                                NULL, static_cast<UInt32 *>(output));
}

IOReturn XenosController::queueQuadScissorStencilBatch(OSObject *owner, void *clientArg, void *input,
                                                        void *output, void *) {
  XenosController *self = static_cast<XenosController *>(owner);
  const XenosQuadScissorStencilBatchUpload *upload =
      static_cast<const XenosQuadScissorStencilBatchUpload *>(input);
  const XenosQuadStencilBatchUpload &batch = upload->batch;
  return self->enqueueQuadBatch(static_cast<XenosQuadClient *>(clientArg), batch.upload.batch,
                                batch.upload.frontAlpha, kXenosQuadUploadScissorStencilBatch,
                                batch.mask, &upload->frontScissor, static_cast<UInt32 *>(output));
}

IOReturn XenosController::queueQuadDepthBatch(OSObject *owner, void *clientArg, void *input,
                                                void *output, void *) {
  XenosController *self = static_cast<XenosController *>(owner);
  const XenosQuadDepthBatchUpload *upload = static_cast<const XenosQuadDepthBatchUpload *>(input);
  return self->enqueueQuadDepthBatch(static_cast<XenosQuadClient *>(clientArg), *upload,
                                     kXenosQuadUploadDepthBatch, static_cast<UInt32 *>(output));
}

IOReturn XenosController::queueQuadRenderTextureBatch(OSObject *owner, void *clientArg, void *input,
                                                        void *output, void *) {
  XenosController *self = static_cast<XenosController *>(owner);
  const XenosQuadDepthBatchUpload *upload = static_cast<const XenosQuadDepthBatchUpload *>(input);
  return self->enqueueQuadDepthBatch(static_cast<XenosQuadClient *>(clientArg), *upload,
                                     kXenosQuadUploadRenderTextureBatch, static_cast<UInt32 *>(output));
}

IOReturn XenosController::queueQuadBilinearBatch(OSObject *owner, void *clientArg, void *input,
                                                   void *output, void *) {
  XenosController *self = static_cast<XenosController *>(owner);
  const XenosQuadDepthBatchUpload *upload = static_cast<const XenosQuadDepthBatchUpload *>(input);
  return self->enqueueQuadDepthBatch(static_cast<XenosQuadClient *>(clientArg), *upload,
                                     kXenosQuadUploadBilinearBatch, static_cast<UInt32 *>(output));
}

IOReturn XenosController::queueQuadMipmapBatch(OSObject *owner, void *clientArg, void *input,
                                                 void *output, void *) {
  XenosController *self = static_cast<XenosController *>(owner);
  const XenosQuadDepthBatchUpload *upload = static_cast<const XenosQuadDepthBatchUpload *>(input);
  return self->enqueueQuadDepthBatch(static_cast<XenosQuadClient *>(clientArg), *upload,
                                     kXenosQuadUploadMipmapBatch, static_cast<UInt32 *>(output));
}

IOReturn XenosController::queueQuadTrilinearBatch(OSObject *owner, void *clientArg, void *input,
                                                 void *output, void *) {
  XenosController *self = static_cast<XenosController *>(owner);
  const XenosQuadDepthBatchUpload *upload = static_cast<const XenosQuadDepthBatchUpload *>(input);
  return self->enqueueQuadDepthBatch(static_cast<XenosQuadClient *>(clientArg), *upload,
                                     kXenosQuadUploadTrilinearBatch, static_cast<UInt32 *>(output));
}

IOReturn XenosController::queueQuadAnisotropicBatch(OSObject *owner, void *clientArg, void *input,
                                                 void *output, void *) {
  XenosController *self = static_cast<XenosController *>(owner);
  const XenosQuadDepthBatchUpload *upload = static_cast<const XenosQuadDepthBatchUpload *>(input);
  return self->enqueueQuadDepthBatch(static_cast<XenosQuadClient *>(clientArg), *upload,
                                     kXenosQuadUploadAnisotropicBatch, static_cast<UInt32 *>(output));
}

IOReturn XenosController::enqueueQuadDepthBatch(XenosQuadClient *client, const XenosQuadDepthBatchUpload &upload,
                                                  UInt32 mode, UInt32 *output) {
  if (upload.depthSplitX > kXenosQuadTargetSide)
    return kIOReturnBadArgument;
  const XenosQuadStencilBatchUpload &batch = upload.clip.batch;
  return enqueueQuadBatch(client, batch.upload.batch, batch.upload.frontAlpha, mode,
                          batch.mask, &upload.clip.frontScissor, output, upload.depthSplitX);
}

IOReturn XenosController::enqueueQuadBatch(XenosQuadClient *client, const XenosQuadBatchUpload &batch,
                                            const UInt8 *frontAlpha, UInt32 mode,
                                            const XenosQuadScissor *frontMask,
                                            const XenosQuadScissor *frontScissor,
                                            UInt32 *output, UInt32 depthSplitX) {
  if (frontScissor && !xenosQuadValidScissor(frontScissor))
    return kIOReturnBadArgument;
  if (frontMask) {
    for (UInt32 index = 0; index < kXenosQuadStencilRects; ++index) {
      if (!xenosQuadValidScissor(&frontMask[index]))
        return kIOReturnBadArgument;
    }
  }
  const IOReturn ready = quadQueueReady(client);
  if (ready != kIOReturnSuccess)
    return ready;

  for (UInt32 index = 0; index < kXenosQuadBatchLayers; ++index) {
    const XenosQuadBatchLayer &layer = batch.layers[index];
    if (layer.kind != kXenosQuadAxisAligned && layer.kind != kXenosQuadRotated45)
      return kIOReturnBadArgument;
    if (!prepareQuadLayer(_quadLayers[index], layer.payload.geometry,
                          layer.payload.rgb, layer.kind == kXenosQuadRotated45))
      return kIOReturnBadArgument;
  }
  if (frontAlpha)
    memcpy(_quadFrontAlpha, frontAlpha, sizeof(_quadFrontAlpha));
  if (frontMask)
    memcpy(_quadFrontMask, frontMask, sizeof(_quadFrontMask));
  if (frontScissor)
    _quadFrontScissor = *frontScissor;
  _quadDepthSplitX = depthSplitX;
  startQuad(client, 0, true, mode, kXenosQuadBatchLayers, output);
  return kIOReturnSuccess;
}

IOReturn XenosController::quadQueueReady(XenosQuadClient *client) {
  if (isInactive() || client->isInactive())
    return kIOReturnNotReady;
  return _quadClient || _quadScratch || _quadCooldown || _surfaceQueueCount
      ? kIOReturnBusy : kIOReturnSuccess;
}

bool XenosController::prepareQuadLayer(QuadLayer &layer, const XenosQuadGeometry &geometry,
                                        const UInt8 *rgb, bool rotated) {
  const bool valid = rotated ? xenosQuadDecodeRotation(&geometry, &layer.rotation) :
                               xenosQuadDecodeGeometry(&geometry, &layer.mapping);
  if (!valid)
    return false;

  // The geometry and texture outlive the RPC and share the work-loop gate.
  layer.payload.geometry = geometry;
  layer.rotated = rotated;
  if (rgb)
    memcpy(layer.payload.rgb, rgb, sizeof(layer.payload.rgb));
  return true;
}

IOReturn XenosController::enqueueQuad(XenosQuadClient *client, const UInt8 *rgb, UInt32 texture,
                                      const XenosQuadGeometry *geometry, UInt32 geometryMode,
                                      UInt32 *output) {
  if (!rgb && texture >= kXenosQuadTextureCount)
    return kIOReturnBadArgument;
  const IOReturn ready = quadQueueReady(client);
  if (ready != kIOReturnSuccess)
    return ready;

  const XenosQuadGeometry requested = geometry ? *geometry :
      xenosQuadMakeGeometry(kQuadOrigin, kQuadOrigin, kQuadOrigin + kQuadSide,
                             kQuadOrigin + kQuadSide, 0, 0, kQuadSide, kQuadSide);
  QuadLayer &layer = _quadLayers[0];
  if (!prepareQuadLayer(layer, requested, rgb, geometryMode == kXenosQuadUploadRotated))
    return kIOReturnBadArgument;

  if (!rgb) {
    for (UInt32 y = 0; y < kQuadSide; ++y) {
      for (UInt32 x = 0; x < kQuadSide; ++x) {
        const UInt32 color = builtInTexel(texture, x, y);
        UInt8 *texel = layer.payload.rgb + (y * kQuadSide + x) * 3;
        texel[0] = (color >> 16) & 0xff;
        texel[1] = (color >> 8) & 0xff;
        texel[2] = color & 0xff;
      }
    }
  }
  startQuad(client, texture, rgb != NULL, geometryMode, 1, output);
  return kIOReturnSuccess;
}

UInt32 XenosController::reserveQuadTicket() {
  if (++_nextQuadTicket == 0)
    ++_nextQuadTicket;
  return _nextQuadTicket;
}

void XenosController::startQuad(XenosQuadClient *client, UInt32 texture, bool uploaded,
                                UInt32 geometryMode, UInt32 layers, UInt32 *output, UInt32 reservedTicket) {
  _quadTicket = reservedTicket ? reservedTicket : reserveQuadTicket();
  _quadClient = client;
  client->retain();
  _quadTexture = texture;
  _quadUploaded = uploaded;
  _quadGeometryMode = geometryMode;
  _quadLayerCount = layers;
  _quadFirstOnly = _quadSecondOnly = _quadOverlap = 0;
  _quadTranslucentOverlap = _quadMaxBlendDelta = 0;
  _quadClippedBack = _quadClippedClear = 0;
  _quadScissorRejectOnly = _quadStencilRejectOnly = _quadBothReject = 0;
  _quadDepthReject = 0;
  _quadSampleChecked = _quadSampleMismatches = 0;
  _quadSampleTexels = _quadSampleGuard = _quadSampleHash = 0;
  _quadSampleMaxDelta = _quadBilinearWitnesses = 0;
  _quadMipChecked = _quadMipMismatches = _quadMipHash = 0;
  _quadMipBaseMatches = _quadMipLevel1Matches = _quadMipLevel1Evidence = 0;
  _quadTrilinearMatches = _quadTrilinearWitnesses = _quadTrilinearMaxDelta = 0;
  _quadAnisoControl = _quadAnisoDark = _quadAnisoLight = _quadAnisoDarkMax = 0;
  _quadAnisoLightMin = 255;
  _quadWait = 0;
  client->_captureQuad = false;
  client->_ticket = _quadTicket;
  bzero(client->_result, sizeof(client->_result));
  *output = _quadTicket;

  setProperty("XenosUserQuadTicket", _quadTicket, 32);
  const char *source = geometryMode == kXenosSurfaceClear ? "Reusable Surface" :
                       geometryMode == kXenosQuadUploadAnisotropicBatch ? "Anisotropic Texture Batch" :
                       geometryMode == kXenosQuadUploadTrilinearBatch ? "Trilinear Texture Batch" :
                       geometryMode == kXenosQuadUploadMipmapBatch ? "Mipmap Texture Batch" :
                       geometryMode == kXenosQuadUploadBilinearBatch ? "Bilinear Texture Batch" :
                       geometryMode == kXenosQuadUploadRenderTextureBatch ? "Render Texture Batch" :
                       geometryMode == kXenosQuadUploadDepthBatch ? "Scissor, Stencil and Depth Batch" :
                       geometryMode == kXenosQuadUploadScissorStencilBatch ? "Scissor and Stencil Batch" :
                       geometryMode == kXenosQuadUploadStencilBatch ? "Stencil Batch" :
                       geometryMode == kXenosQuadUploadScissoredBatch ? "Scissored Batch" :
                       geometryMode == kXenosQuadUploadBlendedBatch ? "Blended Batch" :
                       geometryMode == kXenosQuadUploadBatch ? "Batch" :
                       geometryMode == kXenosQuadUploadRotated ? "Rotated" :
                       geometryMode == kXenosQuadUploadGeometry ? "Geometry" :
                       uploaded ? "Upload" : "Built-in";
  setProperty("XenosUserQuadSource", source);
  if (uploaded)
    removeProperty("XenosUserQuadTexture");
  else
    setProperty("XenosUserQuadTexture", texture, 32);
  setProperty("XenosUserQuadResult", "Pending");
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

bool XenosController::quadSampleAt(const QuadLayer &layer, UInt32 x, UInt32 y,
                                      UInt32 &u, UInt32 &v) const {
  if (layer.rotated) {
    unsigned texelU = 0, texelV = 0;
    if (!xenosQuadRotatedPixel(&layer.rotation, x, y, &texelU, &texelV))
      return false;
    u = texelU;
    v = texelV;
    return true;
  }
  const XenosQuadMapping &m = layer.mapping;
  if (x < m.x0 || x >= m.x1 || y < m.y0 || y >= m.y1)
    return false;
  u = xenosQuadPointSample(m.u0, m.u1, x - m.x0, m.scaleX);
  v = xenosQuadPointSample(m.v0, m.v1, y - m.y0, m.scaleY);
  return true;
}

bool XenosController::quadFrontPassesStencil(UInt32 x, UInt32 y) const {
  for (UInt32 index = 0; index < kXenosQuadStencilRects; ++index) {
    if (xenosQuadInScissor(&_quadFrontMask[index], x, y))
      return true;
  }
  return false;
}

bool XenosController::quadUsesScissor() const {
  return _quadGeometryMode == kXenosQuadUploadScissoredBatch ||
         _quadGeometryMode == kXenosQuadUploadScissorStencilBatch ||
         quadUsesDepth();
}

bool XenosController::quadUsesStencil() const {
  return _quadGeometryMode == kXenosQuadUploadStencilBatch ||
         _quadGeometryMode == kXenosQuadUploadScissorStencilBatch ||
         quadUsesDepth();
}

bool XenosController::quadUsesDepth() const {
  return _quadGeometryMode == kXenosQuadUploadDepthBatch || quadUsesRenderTexture();
}

bool XenosController::quadUsesRenderTexture() const {
  return _quadGeometryMode == kXenosQuadUploadRenderTextureBatch ||
         _quadGeometryMode == kXenosQuadUploadBilinearBatch ||
         _quadGeometryMode == kXenosQuadUploadMipmapBatch ||
         _quadGeometryMode == kXenosQuadUploadTrilinearBatch ||
         _quadGeometryMode == kXenosQuadUploadAnisotropicBatch;
}

bool XenosController::quadUsesBlending() const {
  return _quadGeometryMode == kXenosQuadUploadBlendedBatch ||
         quadUsesScissor() || quadUsesStencil();
}

UInt32 XenosController::quadLayerTexel(UInt32 layer, UInt32 u, UInt32 v) const {
  const UInt32 index = v * kQuadSide + u;
  const UInt8 *rgb = _quadLayers[layer].payload.rgb + 3 * index;
  const UInt32 alpha = layer == 1 && quadUsesBlending()
      ? _quadFrontAlpha[index] : 255;
  return (alpha << 24) | (UInt32(rgb[0]) << 16) | (UInt32(rgb[1]) << 8) | rgb[2];
}

bool XenosController::stepQuad() {
  if (!_quadClient && !_quadScratch) {
    if (!_surfaceQueueCount || _quadCooldown)
      return true;
    startNextSurface();
  }
  const bool drained = readReg32(kXenosRegCpRbReadPtr) == _ringBufferWritePtr &&
                       readReg32(kXenosRegCpRbWritePtr) == _ringBufferWritePtr;
  const bool renderTexture = quadUsesRenderTexture();
  const bool trilinear = _quadGeometryMode == kXenosQuadUploadTrilinearBatch;
  const bool mipmap = trilinear || _quadGeometryMode == kXenosQuadUploadMipmapBatch;
  const bool anisotropic = _quadGeometryMode == kXenosQuadUploadAnisotropicBatch;

  if (!_quadScratch) {
    if (!drained || _markerSubmitAttempts) {
      if (++_quadWait >= kQuadWaitLimit) {
        finishQuad(kXenosQuadUnavailable, 0, 0, 0);
        retireSurfaceQueueEntry(0);
        return true;
      }
      return false;
    }
    const UInt32 budget = mipmap || anisotropic ? kQuadMipmapRingDwords :
                          renderTexture ? kQuadRenderTextureRingDwords : kQuadRingDwords;
    if (_ringBufferWritePtr > kXenosRingbufferLength - budget) {
      while (_ringBufferWritePtr)
        writeRing(2U << 30); // Type-2 NOP up to slot zero.
      syncRingBuffer();
      return false;
    }
    const bool surface = _quadGeometryMode == kXenosSurfaceClear;
    if (!(surface ? submitSurfaceClear() : submitQuad())) {
      finishQuad(kXenosQuadUnavailable, 0, 0, 0);
      retireSurfaceQueueEntry(0);
      return true;
    }
    if (SurfaceQueueRecord *record = surfaceQueueRecord(_surfaceActiveTicket))
      record->state = kXenosSurfaceQueueInFlight;
    _quadWait = 0;
    return false;
  }

  UInt8 *memory = static_cast<UInt8 *>(_quadScratch->getBytesNoCopy());
  const bool surface = _quadGeometryMode == kXenosSurfaceClear;
  const UInt32 fenceOffset = surface ? kSurfaceFenceOffset : kQuadFenceOffset;
  volatile UInt32 *slot = reinterpret_cast<volatile UInt32 *>(memory + fenceOffset);
  flushMarkerCacheLine(slot);
  const UInt32 fence = *slot;
  if (!drained || fence != _quadTicket) {
    if (_quadClient && ++_quadWait >= kQuadWaitLimit)
      finishQuad(kXenosQuadTimedOut, fence, 0, 0);
    // The batch may still write to scratch. Keep it wired and hold new
    // submissions until the fence and the display-state restore have landed.
    return false;
  }

  if (_quadClient && surface) {
    verifySurfaceClear(fence);
  } else if (_quadClient) {
    volatile UInt32 *pixels = reinterpret_cast<volatile UInt32 *>(memory + kQuadReadbackOffset);
    for (UInt32 offset = 0; offset < PAGE_SIZE; offset += CACHELINE_SIZE)
      flushMarkerCacheLine(pixels + offset / sizeof(UInt32));

    UInt32 mismatches = 0;
    UInt32 hash = XENOS_QUAD_HASH_INITIAL;
    const bool blending = quadUsesBlending();
    const bool scissored = quadUsesScissor();
    const bool stenciled = quadUsesStencil();
    const bool combined = scissored && stenciled;
    const bool depth = quadUsesDepth();
    const char *blendLog = anisotropic ? "XenosBatchAnisotropic" :
                           trilinear ? "XenosBatchTrilinear" :
                           mipmap ? "XenosBatchMipmap" :
                           _quadGeometryMode == kXenosQuadUploadBilinearBatch ? "XenosBatchBilinear" :
                           renderTexture ? "XenosBatchRenderTexture" :
                           depth ? "XenosBatchDepth" :
                           combined ? "XenosBatchScissorStencil" :
                           _quadGeometryMode == kXenosQuadUploadStencilBatch ? "XenosBatchStencil" :
                           _quadGeometryMode == kXenosQuadUploadScissoredBatch ? "XenosBatchScissor" :
                           "XenosBatchBlend";
    for (UInt32 y = 0; y < kClearSide; ++y) {
      for (UInt32 x = 0; x < kClearSide; ++x) {
        UInt32 u = 0, v = 0;
        const bool first = quadSampleAt(_quadLayers[0], x, y, u, v);
        const UInt32 destination = first ? quadLayerTexel(0, u, v) : kClearExpected;
        const bool frontGeometry = _quadLayerCount == kXenosQuadBatchLayers &&
                                   quadSampleAt(_quadLayers[1], x, y, u, v);
        const bool scissorReject = frontGeometry && scissored &&
                                   !xenosQuadInScissor(&_quadFrontScissor, x, y);
        const bool stencilReject = frontGeometry && stenciled && !quadFrontPassesStencil(x, y);
        const bool clipped = scissorReject || stencilReject;
        // A pixel center left of the split has an opaque depth nearer than the
        // translucent plane. Pixels without opaque coverage retain clear depth.
        const bool depthReject = frontGeometry && !clipped && depth && first && x < _quadDepthSplitX;
        const bool second = frontGeometry && !clipped && !depthReject;
        UInt32 expected = destination;
        bool translucent = false;
        if (second) {
          const UInt32 source = quadLayerTexel(1, u, v);
          expected = blending ? quadSourceOver(source, destination) : source;
          const UInt32 alpha = source >> 24;
          translucent = blending && alpha > 0 && alpha < 255;
        }
        const UInt32 observed = pixels[resolveTileWord(x, y)];
        if (_quadClient->_captureQuad)
          _quadClient->_quadCapturePixels[y * kClearSide + x] = observed;
        const UInt32 delta = translucent ? quadMaxChannelDelta(observed, expected) : 0;
        const bool matched = translucent ? delta <= kXenosQuadBlendTolerance : observed == expected;
        if (delta > _quadMaxBlendDelta)
          _quadMaxBlendDelta = delta;
        if (blending && !matched && !mismatches) {
          IOLog("%s: first mismatch=(%lu,%lu) expected=%08lx observed=%08lx\n",
                blendLog, x, y, expected, observed);
        }
        mismatches += !matched;
        _quadFirstOnly += matched && first && !second;
        _quadSecondOnly += matched && second && !first;
        _quadOverlap += matched && first && second;
        _quadTranslucentOverlap += matched && first && second && translucent;
        _quadClippedBack += matched && clipped && first;
        _quadClippedClear += matched && clipped && !first;
        _quadDepthReject += matched && depthReject;
        if (combined && matched && clipped) {
          _quadScissorRejectOnly += scissorReject && !stencilReject;
          _quadStencilRejectOnly += stencilReject && !scissorReject;
          _quadBothReject += scissorReject && stencilReject;
        }
        if (renderTexture || first || second)
          hash = quadHashColor(hash, observed);
      }
    }
    if (mipmap)
      verifyQuadMipmapPass(pixels, reinterpret_cast<volatile UInt32 *>(memory + kQuadMipOffset),
                           reinterpret_cast<volatile UInt32 *>(memory + kQuadSampleOffset));
    else if (anisotropic)
      verifyQuadAnisotropicPass(reinterpret_cast<volatile UInt32 *>(memory + kQuadSampleOffset));
    else if (renderTexture)
      verifyQuadTexturePass(pixels, reinterpret_cast<volatile UInt32 *>(memory + kQuadSampleOffset));
    const UInt32 failures = mismatches + _quadMipMismatches + _quadSampleMismatches;
    const bool missingBlend = trilinear && !_quadTrilinearWitnesses;
    finishQuad(failures || missingBlend ? kXenosQuadPixelsFailed : kXenosQuadPassed,
               fence, kClearPixels + _quadMipChecked + _quadSampleChecked, failures, failures ? 0 : hash);
  }
  _quadScratch->release();
  _quadScratch = NULL;
  if (_surfaceJobPixels) {
    _surfaceJobPixels->release();
    _surfaceJobPixels = NULL;
  }
  retireSurfaceQueueEntry(fence);
  return true;
}

void XenosController::verifyQuadTexturePass(const volatile UInt32 *first, volatile UInt32 *sample) {
  for (UInt32 offset = 0; offset < PAGE_SIZE; offset += CACHELINE_SIZE)
    flushMarkerCacheLine(sample + offset / sizeof(UInt32));

  _quadSampleHash = XENOS_QUAD_HASH_INITIAL;
  const bool bilinear = _quadGeometryMode == kXenosQuadUploadBilinearBatch;
  for (UInt32 y = 0; y < kClearSide; ++y) {
    for (UInt32 x = 0; x < kClearSide; ++x) {
      const bool inside = x >= kSampleBorder && x < kClearSide - kSampleBorder &&
                          y >= kSampleBorder && y < kClearSide - kSampleBorder;
      const UInt32 observed = sample[resolveTileWord(x, y)];
      const UInt32 sourceX = kClearSide - 1 - x;
      UInt32 expected = inside ? first[resolveTileWord(sourceX, y)] : kSampleBackground;
      bool matched = observed == expected;
      if (inside && bilinear) {
        const UInt32 neighbors[] = {expected, first[resolveTileWord(sourceX + 1, y)],
                                    first[resolveTileWord(sourceX, y + 1)],
                                    first[resolveTileWord(sourceX + 1, y + 1)]};
        // This path measures one 1/64-texel phase step below programmed UVs.
        expected = quadBilinearColor(neighbors, kSampleOffsetUQuarter * 16 - 1,
                                     kSampleOffsetVQuarter * 16 - 1);
        const UInt32 delta = quadMaxChannelDelta(observed, expected);
        matched = delta <= kXenosQuadBilinearTolerance;
        if (delta > _quadSampleMaxDelta)
          _quadSampleMaxDelta = delta;

        // A matching pixel distinct from all four taps is evidence of filtering.
        bool distinct = matched;
        for (UInt32 index = 0; index < 4; ++index)
          distinct = distinct && quadMaxChannelDelta(observed, neighbors[index]) > kXenosQuadBilinearTolerance;
        _quadBilinearWitnesses += distinct;
      }
      if (!matched && !_quadSampleMismatches) {
        IOLog("%s: sample mismatch=(%lu,%lu) expected=%08lx observed=%08lx\n",
              bilinear ? "XenosBatchBilinear" : "XenosBatchRenderTexture", x, y, expected, observed);
      }
      _quadSampleMismatches += !matched;
      _quadSampleTexels += matched && inside;
      _quadSampleGuard += matched && !inside;
      _quadSampleHash = quadHashColor(_quadSampleHash, observed);
    }
  }
  _quadSampleChecked = kClearPixels;
}

void XenosController::verifyQuadMipmapPass(const volatile UInt32 *base, volatile UInt32 *mip,
                                             volatile UInt32 *sample) {
  for (UInt32 offset = 0; offset < PAGE_SIZE; offset += CACHELINE_SIZE) {
    flushMarkerCacheLine(mip + offset / sizeof(UInt32));
    flushMarkerCacheLine(sample + offset / sizeof(UInt32));
  }

  const bool trilinear = _quadGeometryMode == kXenosQuadUploadTrilinearBatch;
  const char *log = trilinear ? "XenosBatchTrilinear" : "XenosBatchMipmap";
  _quadMipHash = _quadSampleHash = XENOS_QUAD_HASH_INITIAL;
  for (UInt32 y = 0; y < kClearSide; ++y) {
    for (UInt32 x = 0; x < kClearSide; ++x) {
      const UInt32 slot = resolveTileWord(x, y);
      const bool inTail = x >= kMipTailX && y < kMipSide;
      const UInt32 expectedMip = inTail ? base[resolveTileWord(2 * (x - kMipTailX), 2 * y)]
                                         : kSampleBackground;
      const UInt32 mipPixel = mip[slot];
      if (mipPixel != expectedMip) {
        if (!_quadMipMismatches)
          IOLog("%s: tail mismatch=(%lu,%lu) expected=%08lx observed=%08lx\n",
                log, x, y, expectedMip, mipPixel);
        ++_quadMipMismatches;
      }
      _quadMipHash = quadHashColor(_quadMipHash, mipPixel);
      if (trilinear)
        continue;

      const bool level1 = x >= kMipSampleOrigin && x < kMipSampleOrigin + kMipSide &&
                          y >= kMipSampleOrigin && y < kMipSampleOrigin + kMipSide;
      const bool level0 = x >= kBaseSampleOrigin && x < kBaseSampleOrigin + kBaseSampleSide &&
                          y >= kBaseSampleOrigin && y < kBaseSampleOrigin + kBaseSampleSide;
      const UInt32 observed = sample[slot];
      UInt32 expected = kSampleBackground;
      if (level1) {
        const UInt32 u = kMipSide - 1 - (x - kMipSampleOrigin), v = y - kMipSampleOrigin;
        expected = mip[resolveTileWord(kMipTailX + u, v)];
        // At these UVs a direct base lookup lands on the odd texel, while
        // level 1 was prepared from the even texel in each 2x2 footprint.
        _quadMipLevel1Evidence += observed == expected &&
                                  observed != base[resolveTileWord(2 * u + 1, 2 * v + 1)];
      } else if (level0) {
        expected = base[resolveTileWord(x - kBaseSampleOrigin + kBaseSampleU,
                                         y - kBaseSampleOrigin + kBaseSampleV)];
      }

      const bool matched = observed == expected;
      if (!matched) {
        if (!_quadSampleMismatches)
          IOLog("XenosBatchMipmap: sample mismatch=(%lu,%lu) expected=%08lx observed=%08lx\n",
                x, y, expected, observed);
        ++_quadSampleMismatches;
      }
      _quadMipBaseMatches += matched && level0;
      _quadMipLevel1Matches += matched && level1;
      _quadSampleGuard += matched && !level0 && !level1;
      _quadSampleHash = quadHashColor(_quadSampleHash, observed);
    }
  }
  if (trilinear)
    verifyQuadTrilinearSamples(base, mip, sample);
  _quadMipChecked = _quadSampleChecked = kClearPixels;
}

static UInt32 trilinearBilinearSample(const volatile UInt32 *image, UInt32 u, UInt32 v,
                                     UInt32 level, UInt32 tailX) {
  // Scale the base-level 1/64-texel coordinates to the requested mip and
  // subtract half a texel for bilinear filtering. The level-0 control uses
  // the measured extra phase step from the 1:1 path. The level-1 control
  // matched centered weights on the hardware capture.
  const UInt32 center = 32 + (level == 0);
  u = (u >> level) - center;
  v = (v >> level) - center;
  const UInt32 x = tailX + (u >> 6), y = v >> 6;
  const UInt32 taps[] = {image[resolveTileWord(x, y)], image[resolveTileWord(x + 1, y)],
                         image[resolveTileWord(x, y + 1)], image[resolveTileWord(x + 1, y + 1)]};
  return quadBilinearColor(taps, u & 63, v & 63);
}

static UInt32 mipMidpoint(UInt32 level0, UInt32 level1) {
  UInt32 result = 0;
  for (UInt32 shift = 0; shift < 32; shift += 8) {
    const UInt32 sum = ((level0 >> shift) & 0xff) + ((level1 >> shift) & 0xff);
    result |= ((sum + 1) / 2) << shift;
  }
  return result;
}

void XenosController::verifyQuadTrilinearSamples(const volatile UInt32 *base, const volatile UInt32 *mip,
                                                  const volatile UInt32 *sample) {
  for (UInt32 y = 0; y < kTrilinearHeight; ++y) {
    for (UInt32 x = 0; x < kTrilinearWidth; ++x) {
      const UInt32 u = kTrilinearU + x;
      const UInt32 v = kTrilinearV + y;
      const UInt32 drawY = kTrilinearRegionY + y;
      const UInt32 observed0 = sample[resolveTileWord(kTrilinearRegionX[0] + x, drawY)];
      const UInt32 observedMid = sample[resolveTileWord(kTrilinearRegionX[1] + x, drawY)];
      const UInt32 observed1 = sample[resolveTileWord(kTrilinearRegionX[2] + x, drawY)];
      const UInt32 offsetsU = 32 + kTrilinearUQuarter * 16;
      const UInt32 offsetsV = 32 + kTrilinearVQuarter * 16;
      const UInt32 expected0 = trilinearBilinearSample(base, u * 64 + offsetsU, v * 64 + offsetsV, 0, 0);
      const UInt32 expected1 = trilinearBilinearSample(mip, u * 64 + offsetsU, v * 64 + offsetsV, 1, kMipTailX);
      const UInt32 expectedMid = mipMidpoint(observed0, observed1);
      const UInt32 delta0 = quadMaxChannelDelta(observed0, expected0);
      const UInt32 deltaMid = quadMaxChannelDelta(observedMid, expectedMid);
      const UInt32 delta1 = quadMaxChannelDelta(observed1, expected1);
      const bool match0 = delta0 <= kTrilinearTolerance;
      const bool matchMid = deltaMid <= kTrilinearTolerance;
      const bool match1 = delta1 <= kTrilinearTolerance;
      if (!_quadSampleMismatches && (!match0 || !matchMid || !match1)) {
        IOLog("XenosBatchTrilinear: sample=(%lu,%lu) level0=%08lx/%08lx mid=%08lx/%08lx level1=%08lx/%08lx\n",
              x, y, observed0, expected0, observedMid, expectedMid, observed1, expected1);
      }
      _quadSampleMismatches += !match0 + !matchMid + !match1;
      _quadMipBaseMatches += match0;
      _quadTrilinearMatches += matchMid;
      _quadMipLevel1Matches += match1;
      _quadTrilinearWitnesses += match0 && matchMid && match1 &&
          quadMaxChannelDelta(observedMid, observed0) > kTrilinearTolerance &&
          quadMaxChannelDelta(observedMid, observed1) > kTrilinearTolerance;
      if (deltaMid > _quadTrilinearMaxDelta)
        _quadTrilinearMaxDelta = deltaMid;
    }
  }
  for (UInt32 y = 0; y < kClearSide; ++y) {
    for (UInt32 x = 0; x < kClearSide; ++x) {
      const UInt32 observed = sample[resolveTileWord(x, y)];
      _quadSampleHash = quadHashColor(_quadSampleHash, observed);
      bool inside = false;
      for (UInt32 index = 0; index < kTrilinearRegionCount; ++index)
        inside |= y >= kTrilinearRegionY && y < kTrilinearRegionY + kTrilinearHeight &&
                  x >= kTrilinearRegionX[index] && x < kTrilinearRegionX[index] + kTrilinearWidth;
      if (!inside) {
        _quadSampleMismatches += observed != kSampleBackground;
        _quadSampleGuard += observed == kSampleBackground;
      }
    }
  }
}

static UInt32 anisotropicStripeColor(UInt32 x) {
  return kAnisoColorTag | ((x & 1 ? kAnisoLightRed : kAnisoDarkRed) << 16);
}

static bool anisotropicQuadCoversPixel(UInt32 x, UInt32 y, UInt32 left) {
  if (x < left || x >= left + kAnisoWidth)
    return false;
  // Use doubled pixel-center coordinates to account for the 1:4 tilt.
  const int alongY = int(kAnisoSlopeDenominator) * (2 * (int(y) - int(kAnisoTop)) + 1) -
                     (2 * int(x - left) + 1);
  return alongY > 0 && alongY < int(2 * kAnisoSlopeDenominator * kAnisoHeight);
}

void XenosController::verifyQuadAnisotropicPass(volatile UInt32 *sample) {
  for (UInt32 offset = 0; offset < PAGE_SIZE; offset += CACHELINE_SIZE)
    flushMarkerCacheLine(sample + offset / sizeof(UInt32));

  _quadSampleHash = XENOS_QUAD_HASH_INITIAL;
  const UInt32 controlGray = kAnisoColorTag | (kAnisoGrayRed << 16);
  for (UInt32 y = 0; y < kClearSide; ++y) {
    for (UInt32 x = 0; x < kClearSide; ++x) {
      const UInt32 observed = sample[resolveTileWord(x, y)];
      const bool control = anisotropicQuadCoversPixel(x, y, kAnisoLeft[0]);
      const bool stripes = anisotropicQuadCoversPixel(x, y, kAnisoLeft[1]);
      bool matched = observed == kSampleBackground;
      if (control) {
        matched = quadMaxChannelDelta(observed, controlGray) <= kAnisoColorTolerance;
        _quadAnisoControl += matched;
      } else if (stripes) {
        const UInt32 red = (observed >> 16) & 0xff;
        const bool light = (kAnisoU + x - kAnisoLeft[1]) & 1;
        const bool tag = quadMaxChannelDelta(observed & 0xff00ffffU, kAnisoColorTag) <= kAnisoColorTolerance;
        matched = tag && (light ? red >= kAnisoGrayRed + kAnisoMinStripeDelta
                                : red <= kAnisoGrayRed - kAnisoMinStripeDelta);
        if (light) {
          _quadAnisoLight += matched;
          if (red < _quadAnisoLightMin)
            _quadAnisoLightMin = red;
        } else {
          _quadAnisoDark += matched;
          if (red > _quadAnisoDarkMax)
            _quadAnisoDarkMax = red;
        }
      } else {
        _quadSampleGuard += matched;
      }
      if (!matched && !_quadSampleMismatches)
        IOLog("XenosBatchAnisotropic: sample mismatch=(%lu,%lu) observed=%08lx control=%u stripes=%u\n",
              x, y, observed, control, stripes);
      _quadSampleMismatches += !matched;
      _quadSampleHash = quadHashColor(_quadSampleHash, observed);
    }
  }
  _quadSampleChecked = kClearPixels;
}

void XenosController::finishQuad(UInt32 state, UInt32 fence, UInt32 checked,
                                  UInt32 mismatches, UInt32 hash) {
  XenosQuadClient *client = _quadClient;
  client->_result[0] = state;
  client->_result[1] = fence;
  client->_result[2] = checked;
  client->_result[3] = mismatches;
  client->_result[4] = hash;
  if (++_gpuCompletionSequence == 0)
    ++_gpuCompletionSequence;
  XenosGPUCompletion completion = {
      _gpuCompletionSequence, client->_ownerPID, _quadTicket, fence, state, checked, mismatches
  };
  setProperty(XENOS_GPU_COMPLETION_KEY, &completion, sizeof(completion));
  _quadClient = NULL;
  _quadCooldown = kQuadCooldownTicks;
  if (state != kXenosQuadPassed) {
    if (SurfaceQueueRecord *record = surfaceQueueRecord(_surfaceActiveTicket))
      record->state = kXenosSurfaceQueueFailed;
  }
  client->release();

  const char *result = state == kXenosQuadPassed ? "PASS" :
                       state == kXenosQuadPixelsFailed ? "FAIL pixels" :
                       state == kXenosQuadTimedOut ? "FAIL timeout" : "FAIL unavailable";
  setProperty("XenosUserQuadFence", fence, 32);
  setProperty("XenosUserQuadPixelsChecked", checked, 32);
  setProperty("XenosUserQuadMismatches", mismatches, 32);
  setProperty("XenosUserQuadReadbackHash", hash, 32);
  setProperty("XenosUserQuadResult", result);
  if (_quadGeometryMode == kXenosSurfaceClear) {
    IOLog("XenosSurface: %s ticket=%lu fence=%lu pixels=%lu mismatches=%lu argb=%08lx hash=%08lx\n",
          result, _quadTicket, fence, checked, mismatches, _surfaceColor, hash);
  } else if (_quadGeometryMode == kXenosQuadUploadAnisotropicBatch) {
    setProperty("XenosBatchAnisotropicStripePixels", _quadAnisoDark + _quadAnisoLight, 32);
    setProperty("XenosBatchAnisotropicControlPixels", _quadAnisoControl, 32);
    IOLog("XenosBatchAnisotropic: %s ticket=%lu fence=%lu first_pixels=%lu first_mismatches=%lu "
          "sample_pixels=%lu sample_mismatches=%lu control_pixels=%lu dark_pixels=%lu light_pixels=%lu "
          "guard_pixels=%lu dark_max=%lu light_min=%lu first_hash=%08lx sample_hash=%08lx\n",
          result, _quadTicket, fence, checked - _quadSampleChecked, mismatches - _quadSampleMismatches,
          _quadSampleChecked, _quadSampleMismatches, _quadAnisoControl, _quadAnisoDark, _quadAnisoLight,
          _quadSampleGuard, _quadAnisoDarkMax, _quadAnisoLightMin, hash, _quadSampleHash);
  } else if (_quadGeometryMode == kXenosQuadUploadTrilinearBatch) {
    setProperty("XenosBatchTrilinearWitnesses", _quadTrilinearWitnesses, 32);
    setProperty("XenosBatchTrilinearMaxDelta", _quadTrilinearMaxDelta, 32);
    IOLog("XenosBatchTrilinear: %s ticket=%lu fence=%lu first_pixels=%lu first_mismatches=%lu "
          "mip_pixels=%lu mip_mismatches=%lu sample_pixels=%lu sample_mismatches=%lu "
          "level0_pixels=%lu blended_pixels=%lu level1_pixels=%lu guard_pixels=%lu blend_evidence=%lu "
          "max_delta=%lu first_hash=%08lx mip_hash=%08lx sample_hash=%08lx\n",
          result, _quadTicket, fence, checked - _quadMipChecked - _quadSampleChecked,
          mismatches - _quadMipMismatches - _quadSampleMismatches, _quadMipChecked,
          _quadMipMismatches, _quadSampleChecked, _quadSampleMismatches, _quadMipBaseMatches,
          _quadTrilinearMatches, _quadMipLevel1Matches, _quadSampleGuard, _quadTrilinearWitnesses,
          _quadTrilinearMaxDelta, hash, _quadMipHash, _quadSampleHash);
  } else if (_quadGeometryMode == kXenosQuadUploadMipmapBatch) {
    setProperty("XenosBatchMipmapLevel0Pixels", _quadMipBaseMatches, 32);
    setProperty("XenosBatchMipmapLevel1Pixels", _quadMipLevel1Matches, 32);
    setProperty("XenosBatchMipmapLevel1Evidence", _quadMipLevel1Evidence, 32);
    setProperty("XenosBatchMipmapTailMismatches", _quadMipMismatches, 32);
    setProperty("XenosBatchMipmapSampleMismatches", _quadSampleMismatches, 32);
    IOLog("XenosBatchMipmap: %s ticket=%lu fence=%lu first_pixels=%lu first_mismatches=%lu "
          "mip_pixels=%lu mip_mismatches=%lu sample_pixels=%lu sample_mismatches=%lu "
          "level0_pixels=%lu level1_pixels=%lu guard_pixels=%lu level1_evidence=%lu "
          "first_hash=%08lx mip_hash=%08lx sample_hash=%08lx\n",
          result, _quadTicket, fence, checked - _quadMipChecked - _quadSampleChecked,
          mismatches - _quadMipMismatches - _quadSampleMismatches, _quadMipChecked,
          _quadMipMismatches, _quadSampleChecked, _quadSampleMismatches, _quadMipBaseMatches,
          _quadMipLevel1Matches, _quadSampleGuard, _quadMipLevel1Evidence, hash, _quadMipHash, _quadSampleHash);
  } else if (quadUsesRenderTexture()) {
    setProperty("XenosBatchRenderTextureFirstPixels", checked - _quadSampleChecked, 32);
    setProperty("XenosBatchRenderTextureSamplePixels", _quadSampleChecked, 32);
    setProperty("XenosBatchRenderTextureFirstMismatches", mismatches - _quadSampleMismatches, 32);
    setProperty("XenosBatchRenderTextureSampleMismatches", _quadSampleMismatches, 32);
    setProperty("XenosBatchRenderTextureMatchedTexels", _quadSampleTexels, 32);
    setProperty("XenosBatchRenderTextureGuardPixels", _quadSampleGuard, 32);
    setProperty("XenosBatchRenderTextureSampleHash", _quadSampleHash, 32);
    if (_quadGeometryMode == kXenosQuadUploadBilinearBatch) {
      setProperty("XenosBatchBilinearWitnesses", _quadBilinearWitnesses, 32);
      setProperty("XenosBatchBilinearMaxDelta", _quadSampleMaxDelta, 32);
      IOLog("XenosBatchBilinear: %s ticket=%lu fence=%lu first_pixels=%lu first_mismatches=%lu "
            "sample_pixels=%lu sample_mismatches=%lu filtered_pixels=%lu guard_pixels=%lu "
            "bilinear_witnesses=%lu filter_max_delta=%lu first_hash=%08lx sample_hash=%08lx\n",
            result, _quadTicket, fence, checked - _quadSampleChecked, mismatches - _quadSampleMismatches,
            _quadSampleChecked, _quadSampleMismatches, _quadSampleTexels, _quadSampleGuard,
            _quadBilinearWitnesses, _quadSampleMaxDelta, hash, _quadSampleHash);
    } else {
      IOLog("XenosBatchRenderTexture: %s ticket=%lu fence=%lu first_pixels=%lu first_mismatches=%lu "
            "sample_pixels=%lu sample_mismatches=%lu sampled_texels=%lu guard_pixels=%lu "
            "depth_reject=%lu overlap_blended=%lu max_delta=%lu first_hash=%08lx sample_hash=%08lx\n",
            result, _quadTicket, fence, checked - _quadSampleChecked, mismatches - _quadSampleMismatches,
            _quadSampleChecked, _quadSampleMismatches, _quadSampleTexels, _quadSampleGuard,
            _quadDepthReject, _quadTranslucentOverlap, _quadMaxBlendDelta, hash, _quadSampleHash);
    }
  } else if (_quadGeometryMode == kXenosQuadUploadDepthBatch) {
    setProperty("XenosBatchDepthSplitX", _quadDepthSplitX, 32);
    setProperty("XenosBatchDepthRejected", _quadDepthReject, 32);
    setProperty("XenosBatchDepthScissorRejectOnly", _quadScissorRejectOnly, 32);
    setProperty("XenosBatchDepthStencilRejectOnly", _quadStencilRejectOnly, 32);
    setProperty("XenosBatchDepthBothReject", _quadBothReject, 32);
    setProperty("XenosBatchDepthOverlap", _quadTranslucentOverlap, 32);
    setProperty("XenosBatchDepthFrontOnly", _quadSecondOnly, 32);
    setProperty("XenosBatchDepthMaxDelta", _quadMaxBlendDelta, 32);
    IOLog("XenosBatchDepth: %s ticket=%lu draws=%lu stencil_draws=%u fence=%lu pixels=%lu mismatches=%lu "
          "split_x=%lu scissor_reject_only=%lu stencil_reject_only=%lu both_reject=%lu depth_reject=%lu "
          "front_only=%lu overlap_blended=%lu max_delta=%lu hash=%08lx\n",
          result, _quadTicket, _quadLayerCount, unsigned(kXenosQuadStencilRects), fence,
          checked, mismatches, _quadDepthSplitX, _quadScissorRejectOnly, _quadStencilRejectOnly,
          _quadBothReject, _quadDepthReject, _quadSecondOnly, _quadTranslucentOverlap,
          _quadMaxBlendDelta, hash);
  } else if (_quadGeometryMode == kXenosQuadUploadScissorStencilBatch) {
    setProperty("XenosBatchScissorStencilScissorRejectOnly", _quadScissorRejectOnly, 32);
    setProperty("XenosBatchScissorStencilStencilRejectOnly", _quadStencilRejectOnly, 32);
    setProperty("XenosBatchScissorStencilBothReject", _quadBothReject, 32);
    setProperty("XenosBatchScissorStencilOverlap", _quadTranslucentOverlap, 32);
    setProperty("XenosBatchScissorStencilFrontOnly", _quadSecondOnly, 32);
    setProperty("XenosBatchScissorStencilMaxDelta", _quadMaxBlendDelta, 32);
    IOLog("XenosBatchScissorStencil: %s ticket=%lu draws=%lu stencil_draws=%u fence=%lu "
          "pixels=%lu mismatches=%lu scissor_reject_only=%lu stencil_reject_only=%lu "
          "both_reject=%lu front_only=%lu overlap_blended=%lu max_delta=%lu hash=%08lx\n",
          result, _quadTicket, _quadLayerCount, unsigned(kXenosQuadStencilRects), fence,
          checked, mismatches, _quadScissorRejectOnly, _quadStencilRejectOnly, _quadBothReject,
          _quadSecondOnly, _quadTranslucentOverlap, _quadMaxBlendDelta, hash);
  } else if (_quadGeometryMode == kXenosQuadUploadStencilBatch) {
    setProperty("XenosBatchStencilMaskedBack", _quadClippedBack, 32);
    setProperty("XenosBatchStencilMaskedClear", _quadClippedClear, 32);
    setProperty("XenosBatchStencilOverlap", _quadTranslucentOverlap, 32);
    setProperty("XenosBatchStencilFrontOnly", _quadSecondOnly, 32);
    setProperty("XenosBatchStencilMaxDelta", _quadMaxBlendDelta, 32);
    IOLog("XenosBatchStencil: %s ticket=%lu draws=%lu stencil_draws=%u fence=%lu "
          "pixels=%lu mismatches=%lu masked_back=%lu masked_clear=%lu "
          "front_only=%lu overlap_blended=%lu max_delta=%lu hash=%08lx\n",
          result, _quadTicket, _quadLayerCount, unsigned(kXenosQuadStencilRects), fence,
          checked, mismatches, _quadClippedBack, _quadClippedClear, _quadSecondOnly,
          _quadTranslucentOverlap, _quadMaxBlendDelta, hash);
  } else if (_quadGeometryMode == kXenosQuadUploadScissoredBatch) {
    setProperty("XenosBatchScissorClippedBack", _quadClippedBack, 32);
    setProperty("XenosBatchScissorClippedClear", _quadClippedClear, 32);
    setProperty("XenosBatchScissorOverlap", _quadTranslucentOverlap, 32);
    setProperty("XenosBatchScissorFrontOnly", _quadSecondOnly, 32);
    setProperty("XenosBatchScissorMaxDelta", _quadMaxBlendDelta, 32);
    const XenosQuadScissor &rect = _quadFrontScissor;
    IOLog("XenosBatchScissor: %s ticket=%lu draws=%lu fence=%lu pixels=%lu mismatches=%lu "
          "rect=%u,%u,%u,%u clipped_back=%lu clipped_clear=%lu "
          "front_only=%lu overlap_blended=%lu max_delta=%lu hash=%08lx\n",
          result, _quadTicket, _quadLayerCount, fence, checked, mismatches,
          unsigned(rect.left), unsigned(rect.top), unsigned(rect.right), unsigned(rect.bottom),
          _quadClippedBack, _quadClippedClear, _quadSecondOnly, _quadTranslucentOverlap,
          _quadMaxBlendDelta, hash);
  } else if (_quadGeometryMode == kXenosQuadUploadBlendedBatch) {
    setProperty("XenosBatchBlendFirstOnly", _quadFirstOnly, 32);
    setProperty("XenosBatchBlendSecondOnly", _quadSecondOnly, 32);
    setProperty("XenosBatchBlendOverlap", _quadTranslucentOverlap, 32);
    setProperty("XenosBatchBlendMaxDelta", _quadMaxBlendDelta, 32);
    IOLog("XenosBatchBlend: %s ticket=%lu draws=%lu fence=%lu pixels=%lu mismatches=%lu "
          "back_only=%lu front_only=%lu overlap_blended=%lu max_delta=%lu hash=%08lx\n",
          result, _quadTicket, _quadLayerCount, fence, checked, mismatches,
          _quadFirstOnly, _quadSecondOnly, _quadTranslucentOverlap, _quadMaxBlendDelta, hash);
  } else if (_quadGeometryMode == kXenosQuadUploadBatch) {
    setProperty("XenosBatchQuadFirstOnly", _quadFirstOnly, 32);
    setProperty("XenosBatchQuadSecondOnly", _quadSecondOnly, 32);
    setProperty("XenosBatchQuadOverlap", _quadOverlap, 32);
    IOLog("XenosBatchQuad: %s ticket=%lu draws=%lu fence=%lu pixels=%lu mismatches=%lu "
          "first_only=%lu second_only=%lu overlap=%lu hash=%08lx\n",
          result, _quadTicket, _quadLayerCount, fence, checked, mismatches,
          _quadFirstOnly, _quadSecondOnly, _quadOverlap, hash);
  } else if (_quadGeometryMode == kXenosQuadUploadRotated) {
    const XenosQuadRotation &r = _quadLayers[0].rotation;
    IOLog("XenosRotatedQuad: %s ticket=%lu angle=%d origin_q=%u,%u legs_q=%u,%u "
          "uv=%u,%u,%u,%u fence=%lu pixels=%lu mismatches=%lu hash=%08lx\n",
          result, _quadTicket, r.direction * 45, r.originX, r.originY, r.legU, r.legV,
          r.u0, r.v0, r.u1, r.v1, fence, checked, mismatches, hash);
  } else if (_quadGeometryMode == kXenosQuadUploadGeometry) {
    const XenosQuadMapping &m = _quadLayers[0].mapping;
    IOLog("XenosGeometryQuad: %s ticket=%lu rect=%u,%u,%u,%u uv=%u,%u,%u,%u "
          "fence=%lu pixels=%lu mismatches=%lu hash=%08lx\n",
          result, _quadTicket, unsigned(m.x0), unsigned(m.y0), unsigned(m.x1), unsigned(m.y1),
          unsigned(m.u0), unsigned(m.v0), unsigned(m.u1), unsigned(m.v1),
          fence, checked, mismatches, hash);
  } else if (_quadUploaded) {
    IOLog("XenosUploadQuad: %s ticket=%lu bytes=%u fence=%lu pixels=%lu mismatches=%lu hash=%08lx\n",
          result, _quadTicket, unsigned(kXenosQuadUploadBytes), fence, checked, mismatches, hash);
  } else {
    IOLog("XenosUserQuad: %s ticket=%lu texture=%lu fence=%lu pixels=%lu mismatches=%lu\n",
          result, _quadTicket, _quadTexture, fence, checked, mismatches);
  }
}

bool XenosController::ownsSurface(XenosQuadClient *client, UInt32 handle) const {
  return handle && client->_surface && handle == client->_surfaceHandle;
}

static IOBufferMemoryDescriptor *allocateSurfaceBuffer(IOByteCount bytes) {
  IOBufferMemoryDescriptor *buffer = IOBufferMemoryDescriptor::withOptions(
      kIOMemoryPhysicallyContiguous, bytes, PAGE_SIZE);
  if (!buffer)
    return NULL;
  IOByteCount length = 0;
  const IOPhysicalAddress physical = buffer->getPhysicalSegment(0, &length);
  if (!buffer->getBytesNoCopy() || !physical || length < bytes ||
      (physical & (PAGE_SIZE - 1)) || physical > kDisplayMemoryStart - bytes) {
    buffer->release();
    return NULL;
  }
  return buffer;
}

IOReturn XenosController::createClientSurface(OSObject *owner, void *clientArg, void *widthArg,
                                               void *heightArg, void *handleOut) {
  XenosController *self = static_cast<XenosController *>(owner);
  XenosQuadClient *client = static_cast<XenosQuadClient *>(clientArg);
  if (self->isInactive() || client->isInactive())
    return kIOReturnNotReady;
  if (reinterpret_cast<uintptr_t>(widthArg) != kXenosSurfaceSide ||
      reinterpret_cast<uintptr_t>(heightArg) != kXenosSurfaceSide)
    return kIOReturnBadArgument;
  if (client->_surface)
    return kIOReturnBusy;
  if (self->_surfaceCount >= kSurfaceAllocationLimit)
    return kIOReturnNoResources;

  IOBufferMemoryDescriptor *surface = allocateSurfaceBuffer(kSurfaceStorageBytes);
  if (!surface)
    return kIOReturnNoResources;
  // A distinct allocation makes the mapping exactly one GPU pixel page.
  // The scratch space and the GPU fence remain kernel-only.
  IOBufferMemoryDescriptor *pixels = allocateSurfaceBuffer(PAGE_SIZE);
  if (!pixels) {
    surface->release();
    return kIOReturnNoResources;
  }

  if (++self->_surfaceSerial == 0)
    ++self->_surfaceSerial;
  ++self->_surfaceCount;
  client->_surface = surface;
  client->_surfacePixels = pixels;
  client->_surfaceHandle = self->_surfaceSerial;
  client->_surfaceTicket = 0;
  client->_surfaceReady = false;
  *static_cast<UInt32 *>(handleOut) = client->_surfaceHandle;
  return kIOReturnSuccess;
}

IOReturn XenosController::queueSurfaceClear(OSObject *owner, void *clientArg, void *handleArg,
                                             void *colorArg, void *ticketOut) {
  XenosController *self = static_cast<XenosController *>(owner);
  XenosQuadClient *client = static_cast<XenosQuadClient *>(clientArg);
  const UInt32 handle = static_cast<UInt32>(reinterpret_cast<uintptr_t>(handleArg));
  const UInt32 argb = static_cast<UInt32>(reinterpret_cast<uintptr_t>(colorArg));
  if (!self->ownsSurface(client, handle) || (argb >> 24) != 0xff)
    return kIOReturnBadArgument;
  const IOReturn ready = self->quadQueueReady(client);
  if (ready != kIOReturnSuccess)
    return ready;

  self->_surfaceColor = argb;
  self->startQuad(client, 0, true, kXenosSurfaceClear, 0, static_cast<UInt32 *>(ticketOut));
  client->_surfaceReady = false;
  client->_surfaceTicket = self->_quadTicket;
  return kIOReturnSuccess;
}

IOReturn XenosController::readClientSurface(OSObject *owner, void *clientArg, void *input,
                                             void *output, void *) {
  XenosController *self = static_cast<XenosController *>(owner);
  XenosQuadClient *client = static_cast<XenosQuadClient *>(clientArg);
  const XenosSurfaceReadRequest &request = *static_cast<const XenosSurfaceReadRequest *>(input);
  if (self->isInactive() || client->isInactive())
    return kIOReturnNotReady;
  if (!self->ownsSurface(client, request.handle) ||
      request.firstRow > kXenosSurfaceSide - kXenosSurfaceReadRows)
    return kIOReturnBadArgument;
  if (request.ticket != client->_surfaceTicket)
    return kIOReturnBadArgument;
  if (!client->_surfaceReady)
    return kIOReturnNotReady;

  const volatile UInt32 *tiled =
      static_cast<const volatile UInt32 *>(client->_surfacePixels->getBytesNoCopy());
  XenosSurfaceReadback &readback = *static_cast<XenosSurfaceReadback *>(output);
  for (UInt32 row = 0; row < kXenosSurfaceReadRows; ++row)
    for (UInt32 x = 0; x < kXenosSurfaceSide; ++x)
      readback.argb[row * kXenosSurfaceSide + x] = tiled[resolveTileWord(x, request.firstRow + row)];
  return kIOReturnSuccess;
}

void XenosController::dropSurface(XenosQuadClient *client) {
  if (client->_surface) {
    if (_drawableGrantPixels == client->_surfacePixels)
      revokeDrawableGrant();
    client->_surfacePixels->release();
    client->_surface->release();
    client->_surface = client->_surfacePixels = NULL;
    --_surfaceCount;
  }
  client->_surfaceHandle = client->_surfaceTicket = 0;
  client->_surfaceReady = false;
}

IOReturn XenosController::releaseClientSurface(OSObject *owner, void *clientArg, void *handleArg,
                                                 void *, void *) {
  XenosController *self = static_cast<XenosController *>(owner);
  XenosQuadClient *client = static_cast<XenosQuadClient *>(clientArg);
  if (!self->ownsSurface(client, static_cast<UInt32>(reinterpret_cast<uintptr_t>(handleArg))))
    return kIOReturnBadArgument;
  if ((self->_quadClient == client && self->_quadGeometryMode == kXenosSurfaceClear) ||
      self->surfaceQueueIndex(client) != self->_surfaceQueueCount)
    return kIOReturnBusy;
  self->dropSurface(client);
  return kIOReturnSuccess;
}

IOReturn XenosController::closeClientSurface(OSObject *owner, void *clientArg, void *, void *, void *) {
  XenosController *self = static_cast<XenosController *>(owner);
  XenosQuadClient *client = static_cast<XenosQuadClient *>(clientArg);
  self->closeQueuedSurface(client);
  self->dropSurface(client);
  if (client->_drawableView) {
    client->_drawableView->release();
    client->_drawableView = NULL;
    --self->_drawableReaders;
  }
  return kIOReturnSuccess;
}

bool XenosController::submitSurfaceClear() {
  if (!_quadClient->_surface || !_fbWidth || !_fbHeight || _fbWidth > 1920 || _fbHeight > 1080)
    return false;

  // The work loop keeps a separate reference until the fence lands, including
  // when the user closes a connection or gives up waiting.
  IOByteCount length = 0;
  const UInt32 physical = static_cast<UInt32>(_quadClient->_surface->getPhysicalSegment(0, &length));
  _quadScratch = _quadClient->_surface;
  _quadScratch->retain();
  _surfaceJobPixels = _quadClient->_surfacePixels;
  _surfaceJobPixels->retain();
  const UInt32 readback = static_cast<UInt32>(_surfaceJobPixels->getPhysicalSegment(0, &length));
  UInt8 *memory = static_cast<UInt8 *>(_quadScratch->getBytesNoCopy());
  prepareQuadReadback(static_cast<UInt8 *>(_surfaceJobPixels->getBytesNoCopy()));
  volatile UInt32 *marker = reinterpret_cast<volatile UInt32 *>(memory + kSurfaceFenceOffset);
  *marker = 0;
  flushMarkerCacheLine(marker);

  const UInt32 savedClear = readReg32(4 * kXenosColorClear);
  const UInt32 savedClearLo = readReg32(4 * (kXenosColorClear + 1));
  const UInt32 savedCoherDest = readReg32(4 * kXenosCoherDestBase1);
  const UInt32 edramColor = (_surfaceColor & 0xff00ff00U) |
                           ((_surfaceColor & 0x00ff0000U) >> 16) |
                           ((_surfaceColor & 0x000000ffU) << 16);
  writeRingPacket0(kXenosWaitUntil, 1);
  writeRing(kWait3dIdleClean);
  setGpuViewport(kXenosSurfaceSide, kXenosSurfaceSide);
  setGpuScissors(kXenosSurfaceSide, kXenosSurfaceSide);
  clearQuadSampleTarget(physical, edramColor);
  resolveQuadColor(readback);
  flushQuadResolve(readback);

  writeRingRegister(kXenosCoherDestBase1, savedCoherDest);
  writeRingPacket0(kXenosColorClear, 2);
  writeRing(savedClear);
  writeRing(savedClearLo);
  writeRingPacket0(REG_A2XX_RB_SURFACE_INFO, 2);
  writeRing(ALIGN(_fbWidth, 32));
  writeRing(k_8_8_8_8 << 16);
  restoreQuadDisplayAndFence(physical + kSurfaceFenceOffset);
  return true;
}

void XenosController::verifySurfaceClear(UInt32 fence) {
  volatile UInt32 *pixels = static_cast<volatile UInt32 *>(_surfaceJobPixels->getBytesNoCopy());
  for (UInt32 offset = 0; offset < PAGE_SIZE; offset += CACHELINE_SIZE)
    flushMarkerCacheLine(pixels + offset / sizeof(UInt32));

  UInt32 mismatches = 0, hash = XENOS_QUAD_HASH_INITIAL;
  for (UInt32 y = 0; y < kXenosSurfaceSide; ++y) {
    for (UInt32 x = 0; x < kXenosSurfaceSide; ++x) {
      const UInt32 observed = pixels[resolveTileWord(x, y)];
      if (!mismatches && observed != _surfaceColor)
        IOLog("XenosSurface: mismatch=(%lu,%lu) expected=%08lx observed=%08lx\n",
              x, y, _surfaceColor, observed);
      mismatches += observed != _surfaceColor;
      hash = quadHashColor(hash, observed);
    }
  }
  if (_quadClient->_surface == _quadScratch)
    _quadClient->_surfaceReady = true;
  finishQuad(mismatches ? kXenosQuadPixelsFailed : kXenosQuadPassed, fence, kClearPixels, mismatches, hash);
}

void XenosController::revokeDrawableGrant() {
  _drawableGrantPixels = NULL;
  bzero(&_drawableGrantToken, sizeof(_drawableGrantToken));
}

IOReturn XenosController::grantDrawable(OSObject *owner, void *clientArg, void *handleArg,
                                        void *tokenOut, void *) {
  XenosController *self = static_cast<XenosController *>(owner);
  XenosQuadClient *client = static_cast<XenosQuadClient *>(clientArg);
  if (self->isInactive() || client->isInactive())
    return kIOReturnNotReady;
  if (!self->ownsSurface(client, static_cast<UInt32>(reinterpret_cast<uintptr_t>(handleArg))))
    return kIOReturnBadArgument;
  if (!client->_surfaceReady)
    return kIOReturnNotReady;
  if (self->_drawableGrantPixels)
    return kIOReturnBusy;

  read_random(&self->_drawableGrantToken, sizeof(self->_drawableGrantToken));
  self->_drawableGrantPixels = client->_surfacePixels;
  *static_cast<XenosDrawableToken *>(tokenOut) = self->_drawableGrantToken;
  return kIOReturnSuccess;
}

IOReturn XenosController::openDrawable(OSObject *owner, void *clientArg, void *tokenArg,
                                       void *, void *) {
  XenosController *self = static_cast<XenosController *>(owner);
  XenosQuadClient *client = static_cast<XenosQuadClient *>(clientArg);
  if (self->isInactive() || client->isInactive())
    return kIOReturnNotReady;
  if (!client->_drawableReader)
    return kIOReturnUnsupported;
  if (client->_drawableView)
    return kIOReturnBusy;

  const XenosDrawableToken &token = *static_cast<const XenosDrawableToken *>(tokenArg);
  UInt32 difference = 0;
  for (UInt32 word = 0; word < kXenosDrawableTokenWords; ++word)
    difference |= token.words[word] ^ self->_drawableGrantToken.words[word];
  if (!self->_drawableGrantPixels || difference)
    return kIOReturnNotPermitted;
  if (self->_drawableReaders >= kSurfaceAllocationLimit)
    return kIOReturnNoResources;

  client->_drawableView = self->_drawableGrantPixels;
  client->_drawableView->retain();
  ++self->_drawableReaders;
  self->revokeDrawableGrant();
  return kIOReturnSuccess;
}

IOReturn XenosController::getDrawableView(OSObject *owner, void *clientArg, void *options,
                                          void *descriptor, void *) {
  XenosController *self = static_cast<XenosController *>(owner);
  XenosQuadClient *client = static_cast<XenosQuadClient *>(clientArg);
  if (self->isInactive() || client->isInactive() || !client->_drawableReader || !client->_drawableView)
    return kIOReturnNotReady;
  client->_drawableView->retain();
  *static_cast<IOOptionBits *>(options) = kIOMapReadOnly;
  *static_cast<IOMemoryDescriptor **>(descriptor) = client->_drawableView;
  return kIOReturnSuccess;
}

XenosController::SurfaceQueueRecord *XenosController::surfaceQueueRecord(UInt32 ticket) {
  SurfaceQueueRecord &record = _surfaceRecords[ticket % kSurfaceQueueHistory];
  return ticket && record.ticket == ticket ? &record : NULL;
}

UInt32 XenosController::surfaceQueueIndex(XenosQuadClient *client) const {
  for (UInt32 index = 0; index < _surfaceQueueCount; ++index)
    if (_surfaceQueue[index].client == client)
      return index;
  return _surfaceQueueCount;
}

XenosController::SurfaceQueueEntry XenosController::removeSurfaceQueueEntry(UInt32 index) {
  const SurfaceQueueEntry entry = _surfaceQueue[index];
  --_surfaceQueueCount;
  for (; index < _surfaceQueueCount; ++index)
    _surfaceQueue[index] = _surfaceQueue[index + 1];
  return entry;
}

IOReturn XenosController::enqueueSurfaceClear(OSObject *owner, void *clientArg, void *handleArg,
                                               void *colorArg, void *output) {
  XenosController *self = static_cast<XenosController *>(owner);
  XenosQuadClient *client = static_cast<XenosQuadClient *>(clientArg);
  if (self->isInactive() || client->isInactive())
    return kIOReturnNotReady;
  const UInt32 handle = static_cast<UInt32>(reinterpret_cast<uintptr_t>(handleArg));
  const UInt32 argb = static_cast<UInt32>(reinterpret_cast<uintptr_t>(colorArg));
  if (!self->ownsSurface(client, handle) || (argb >> 24) != 0xff)
    return kIOReturnBadArgument;
  if (self->_surfaceQueueCount == kSurfaceQueueCapacity || self->_quadClient == client ||
      self->_quadScratch == client->_surface ||
      self->surfaceQueueIndex(client) != self->_surfaceQueueCount)
    return kIOReturnBusy;

  const UInt32 ticket = self->reserveQuadTicket();
  const UInt32 ahead = self->_surfaceQueueCount + (self->_quadClient || self->_quadScratch ? 1 : 0);
  SurfaceQueueRecord &record = self->_surfaceRecords[ticket % kSurfaceQueueHistory];
  bzero(&record, sizeof(record));
  record.ticket = ticket;
  record.handle = handle;
  record.state = kXenosSurfaceQueueWaiting;
  const SurfaceQueueEntry entry = {client, ticket, argb};
  client->retain();
  self->_surfaceQueue[self->_surfaceQueueCount++] = entry;
  client->_surfaceReady = false;
  client->_surfaceTicket = client->_ticket = ticket;
  bzero(client->_result, sizeof(client->_result));
  UInt32 *values = static_cast<UInt32 *>(output);
  values[0] = ticket;
  values[1] = ahead;
  IOLog("XenosSurfaceQueue: accepted ticket=%lu handle=%lu ahead=%lu\n", ticket, handle, ahead);
  return kIOReturnSuccess;
}

IOReturn XenosController::readSurfaceQueue(OSObject *owner, void *clientArg, void *input,
                                            void *output, void *) {
  XenosController *self = static_cast<XenosController *>(owner);
  XenosQuadClient *client = static_cast<XenosQuadClient *>(clientArg);
  if (self->isInactive() || client->isInactive())
    return kIOReturnNotReady;
  const UInt32 ticket = *static_cast<const UInt32 *>(input);
  XenosSurfaceQueueInfo &info = *static_cast<XenosSurfaceQueueInfo *>(output);
  bzero(&info, sizeof(info));
  if (ticket) {
    const SurfaceQueueRecord *record = self->surfaceQueueRecord(ticket);
    if (!record)
      return kIOReturnNotFound;
    info.handle = record->handle;
    info.state = record->state;
    info.fence = record->fence;
    info.completion = record->completion;
    info.ownerClosed = record->ownerClosed;
  }
  info.liveSurfaces = self->_surfaceCount;
  info.pending = self->_surfaceQueueCount;
  info.activeTicket = self->_surfaceActiveTicket;
  return kIOReturnSuccess;
}

void XenosController::closeQueuedSurface(XenosQuadClient *client) {
  const UInt32 index = surfaceQueueIndex(client);
  SurfaceQueueRecord *record = surfaceQueueRecord(client->_surfaceTicket);
  if (record && record->handle == client->_surfaceHandle)
    record->ownerClosed = 1;
  if (record && record->ticket == _surfaceActiveTicket && _quadClient == client && !_quadScratch) {
    // A job selected by the scheduler can still be waiting for ring space.
    // It can be cancelled until the GPU takes a separate buffer reference.
    finishQuad(kXenosQuadUnavailable, 0, 0, 0);
    record->state = kXenosSurfaceQueueCancelled;
    retireSurfaceQueueEntry(0);
  }
  if (index < _surfaceQueueCount) {
    const SurfaceQueueEntry entry = removeSurfaceQueueEntry(index);
    surfaceQueueRecord(entry.ticket)->state = kXenosSurfaceQueueCancelled;
    IOLog("XenosSurfaceQueue: cancelled ticket=%lu handle=%lu\n", entry.ticket, client->_surfaceHandle);
    entry.client->release();
  }
}

void XenosController::startNextSurface() {
  const SurfaceQueueEntry entry = removeSurfaceQueueEntry(0);
  _surfaceColor = entry.argb;
  _surfaceActiveTicket = entry.ticket;
  UInt32 ticket = 0;
  startQuad(entry.client, 0, true, kXenosSurfaceClear, 0, &ticket, entry.ticket);
  entry.client->release();
}

void XenosController::retireSurfaceQueueEntry(UInt32 fence) {
  SurfaceQueueRecord *record = surfaceQueueRecord(_surfaceActiveTicket);
  if (!record)
    return;
  if (fence == record->ticket) {
    record->fence = fence;
    record->completion = ++_surfaceCompletion;
    if (record->state != kXenosSurfaceQueueFailed)
      record->state = kXenosSurfaceQueueComplete;
  }
  IOLog("XenosSurfaceQueue: retired ticket=%lu handle=%lu state=%lu fence=%lu completion=%lu\n",
        record->ticket, record->handle, record->state, record->fence, record->completion);
  _surfaceActiveTicket = 0;
}

bool XenosController::submitQuad() {
  // Use spare eDRAM tiles beyond the largest supported desktop surface.
  if (!_fbWidth || !_fbHeight || _fbWidth > 1920 || _fbHeight > 1080)
    return false;

  const bool renderTexture = quadUsesRenderTexture();
  const bool bilinear = _quadGeometryMode == kXenosQuadUploadBilinearBatch;
  const bool trilinear = _quadGeometryMode == kXenosQuadUploadTrilinearBatch;
  const bool mipmap = trilinear || _quadGeometryMode == kXenosQuadUploadMipmapBatch;
  const bool anisotropic = _quadGeometryMode == kXenosQuadUploadAnisotropicBatch;
  const UInt32 scratchBytes = anisotropic ? kQuadAnisoTextureOffset + PAGE_SIZE :
                              mipmap ? kQuadMipOffset + PAGE_SIZE :
                              renderTexture ? kQuadSampleOffset + PAGE_SIZE :
                              (kQuadPagesBeforeTextures + _quadLayerCount) * PAGE_SIZE;
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
  const UInt32 textures = base + kQuadTextureOffset;
  const bool stencil = quadUsesStencil();
  const bool depth = quadUsesDepth();
  for (UInt32 index = 0; index < _quadLayerCount; ++index) {
    const QuadLayer &layer = _quadLayers[index];
    UInt8 *geometry = memory + kQuadGeometryOffset + index * kQuadGeometryStride;
    UInt32 *positions = reinterpret_cast<UInt32 *>(geometry);
    packQuadGeometry(layer.payload.geometry, positions,
                     reinterpret_cast<UInt32 *>(geometry + CACHELINE_SIZE), layer.rotated);
    if (depth)
      packQuadDepth(layer.payload.geometry, positions, layer.rotated, index == 0, _quadDepthSplitX);

    UInt32 *texels = reinterpret_cast<UInt32 *>(memory + kQuadTextureOffset + index * PAGE_SIZE);
    for (UInt32 y = 0; y < kClearSide; ++y)
      for (UInt32 x = 0; x < kClearSide; ++x)
        texels[resolveTileWord(x, y)] = quadLayerTexel(index, x % kQuadSide, y % kQuadSide);
  }
  if (stencil) {
    for (UInt32 index = 0; index < kXenosQuadStencilRects; ++index) {
      const XenosQuadScissor &rect = _quadFrontMask[index];
      const XenosQuadGeometry geometry = xenosQuadMakeGeometry(rect.left, rect.top, rect.right,
                                                              rect.bottom, 0, 0, kQuadSide, kQuadSide);
      UInt8 *slot = memory + kQuadGeometryOffset + (_quadLayerCount + index) * kQuadGeometryStride;
      packQuadGeometry(geometry, reinterpret_cast<UInt32 *>(slot),
                       reinterpret_cast<UInt32 *>(slot + CACHELINE_SIZE), false);
    }
  }
  const UInt32 geometryCount = _quadLayerCount + (stencil ? kXenosQuadStencilRects : 0);
  const UInt32 sampleGeometryOffset = geometryCount * kQuadGeometryStride;
  UInt8 *sampleGeometrySlot = memory + kQuadGeometryOffset + sampleGeometryOffset;
  if (anisotropic) {
    for (UInt32 index = 0; index < kAnisoRegionCount; ++index) {
      const UInt32 left = kAnisoLeft[index];
      XenosQuadGeometry quad = xenosQuadMakeGeometry(left, kAnisoTop, left + kAnisoWidth,
          kAnisoTop + kAnisoHeight, kAnisoU, 0, kAnisoU + kAnisoWidth, kAnisoVSpan);
      quad.vertices[1].y += kAnisoWidth / kAnisoSlopeDenominator;
      quad.vertices[3].y += kAnisoWidth / kAnisoSlopeDenominator;
      packQuadSampleQuad(sampleGeometrySlot + index * kQuadGeometryStride, quad, 0, 0);
    }
    volatile UInt32 *striped = reinterpret_cast<volatile UInt32 *>(memory + kQuadAnisoTextureOffset);
    volatile UInt32 *mip = reinterpret_cast<volatile UInt32 *>(memory + kQuadMipOffset);
    for (UInt32 y = 0; y < kClearSide; ++y) {
      for (UInt32 x = 0; x < kClearSide; ++x) {
        const UInt32 texel = resolveTileWord(x, y);
        striped[texel] = anisotropicStripeColor(x);
        // Box-filtering each adjacent red pair yields a constant 16x16 mip.
        mip[texel] = x >= kMipTailX && y < kMipSide
            ? kAnisoColorTag | (kAnisoGrayRed << 16) : kClearSentinel;
      }
    }
    syncData(mip, 2 * PAGE_SIZE);
  } else if (mipmap) {
    // The 16x16 level occupies (16,0) in the packed tail of a 32x32 texture.
    const XenosQuadGeometry buildMip =
        xenosQuadMakeGeometry(kMipTailX, 0, kMipTailX + kMipSide, kMipSide, 0, 0, kClearSide, kClearSide);
    packQuadSampleQuad(sampleGeometrySlot, buildMip, -1, -1);
    if (trilinear) {
      for (UInt32 index = 0; index < kTrilinearRegionCount; ++index) {
        const UInt32 left = kTrilinearRegionX[index];
        const XenosQuadGeometry region = xenosQuadMakeGeometry(
            left, kTrilinearRegionY, left + kTrilinearWidth, kTrilinearRegionY + kTrilinearHeight,
            kTrilinearU, kTrilinearV, kTrilinearU + kTrilinearWidth, kTrilinearV + kTrilinearHeight);
        packQuadSampleQuad(sampleGeometrySlot + (index + 1) * kQuadGeometryStride, region,
                           kTrilinearUQuarter, kTrilinearVQuarter);
      }
    } else {
      const UInt32 mipSampleEnd = kMipSampleOrigin + kMipSide;
      const XenosQuadGeometry sampleMip =
          xenosQuadMakeGeometry(kMipSampleOrigin, kMipSampleOrigin, mipSampleEnd, mipSampleEnd,
                                kClearSide, 0, 0, kClearSide);
      const UInt32 baseSampleEnd = kBaseSampleOrigin + kBaseSampleSide;
      const XenosQuadGeometry sampleBase =
          xenosQuadMakeGeometry(kBaseSampleOrigin, kBaseSampleOrigin, baseSampleEnd, baseSampleEnd,
                                kBaseSampleU, kBaseSampleV, kBaseSampleU + kBaseSampleSide,
                                kBaseSampleV + kBaseSampleSide);
      packQuadSampleQuad(sampleGeometrySlot + kQuadGeometryStride, sampleMip, 1, 1);
      packQuadSampleQuad(sampleGeometrySlot + 2 * kQuadGeometryStride, sampleBase, 0, 0);
    }
    prepareQuadReadback(memory + kQuadMipOffset);
  } else if (renderTexture) {
    const UInt32 farEdge = kClearSide - kSampleBorder;
    const XenosQuadGeometry sampleGeometry =
        xenosQuadMakeGeometry(kSampleBorder, kSampleBorder, farEdge, farEdge,
                              farEdge, kSampleBorder, kSampleBorder, farEdge);
    packQuadSampleQuad(sampleGeometrySlot, sampleGeometry, bilinear ? kSampleOffsetUQuarter : 0,
                       bilinear ? kSampleOffsetVQuarter : 0);
  }
  if (renderTexture)
    prepareQuadReadback(memory + kQuadSampleOffset);
  const UInt32 sampleQuads = anisotropic ? kAnisoRegionCount :
                             trilinear ? 1 + kTrilinearRegionCount :
                             mipmap ? 3 : renderTexture ? 1 : 0;
  syncData(memory + kQuadGeometryOffset,
           sampleGeometryOffset + sampleQuads * kQuadGeometryStride);
  syncData(memory + kQuadTextureOffset, _quadLayerCount * PAGE_SIZE);
  prepareQuadReadback(memory + kQuadReadbackOffset);
  volatile UInt32 *marker = reinterpret_cast<volatile UInt32 *>(memory + kQuadFenceOffset);
  *marker = 0;
  flushMarkerCacheLine(marker);

  const UInt32 savedClear = readReg32(4 * kXenosColorClear);
  const UInt32 savedClearLo = readReg32(4 * (kXenosColorClear + 1));
  const UInt32 savedCoherDest = readReg32(4 * kXenosCoherDestBase1);
  const bool blending = quadUsesBlending();
  const bool scissored = quadUsesScissor();
  const UInt32 savedBlend = blending ? readReg32(4 * REG_A2XX_RB_BLEND_CONTROL) : 0;
  const UInt32 savedColorControl = blending ? readReg32(4 * REG_A2XX_RB_COLORCONTROL) : 0;
  const UInt32 savedDepthClear = stencil ? readReg32(4 * kXenosDepthClear) : 0;
  const UInt32 savedDepthInfo = stencil ? readReg32(4 * REG_A2XX_RB_DEPTH_INFO) : 0;
  const UInt32 savedDepthControl = stencil ? readReg32(4 * REG_A2XX_RB_DEPTHCONTROL) : 0;
  const UInt32 savedHiZ = stencil ? readReg32(4 * kXenosHiZControl) : 0;
  const UInt32 savedStencilRef = stencil ? readReg32(4 * REG_A2XX_RB_STENCILREFMASK) : 0;
  const UInt32 savedColorMask = stencil ? readReg32(4 * REG_A2XX_RB_COLOR_MASK) : 0;
  const UInt32 savedClip = depth ? readReg32(4 * REG_A2XX_PA_CL_CLIP_CNTL) : 0;
  const UInt32 savedVte = depth ? readReg32(4 * REG_A2XX_PA_CL_VTE_CNTL) : 0;
  const UInt32 savedZScale = depth ? readReg32(4 * REG_A2XX_PA_CL_VPORT_ZSCALE) : 0;
  const UInt32 savedZOffset = depth ? readReg32(4 * REG_A2XX_PA_CL_VPORT_ZOFFSET) : 0;
  writeRingPacket0(kXenosWaitUntil, 1);
  writeRing(kWait3dIdleClean);
  writeRingPacket0(REG_A2XX_RB_SURFACE_INFO, stencil ? 3 : 2);
  writeRing(kClearEdRamPitch | (stencil ? kClearEdRamPitch << 18 : 0));
  writeRing((k_8_8_8_8 << 16) | kClearTile);
  if (stencil) {
    writeRing(kStencilTile);
    writeRingRegister(kXenosHiZControl, 0);
    writeRingRegister(REG_A2XX_RB_STENCILREFMASK, A2XX_RB_STENCILREFMASK_STENCILWRITEMASK(0xff));
    writeRingRegister(REG_A2XX_RB_DEPTHCONTROL, kStencilClearControl);
    writeRingRegister(kXenosDepthClear, kStencilClear);
  }
  setGpuViewport(kClearSide, kClearSide);
  setGpuScissors(kClearSide, kClearSide);
  setGpuEdRamMode(EDRAM_COPY);
  writeRingPacket0(kXenosColorClear, 2);
  writeRing(kClearColor);
  writeRing(kClearColor);

  const UInt32 format = kResolveDisplayFormat | kMarkerSwap8In32;
  const UInt32 clearFlags = kResolveColorClear | (stencil ? kResolveDepthClear : 0);
  setResolveTarget(base, kClearSide, kClearSide, kResolveConvert | clearFlags, format);
  drawResolveRectangle();
  writeRingPacket0(kXenosWaitUntil, 1);
  writeRing(kWait3dIdleClean);

  syncGpuMemory(vertices, (1 + _quadLayerCount) * PAGE_SIZE, kCoherVertexAndTextureCaches);
  writeRingPacket0(REG_A2XX_RB_COPY_CONTROL, 1);
  writeRing(0);
  writeRingPacket3(CP_INVALIDATE_STATE, 1);
  writeRing(0x00000200);
  setGpuTexturePixelShader(true);
  setGpuEdRamMode(COLOR_DEPTH);
  if (depth) {
    writeRingRegister(REG_A2XX_PA_CL_CLIP_CNTL,
                      savedClip | A2XX_PA_CL_CLIP_CNTL_DX_CLIP_SPACE_DEF(DXCLIP_DIRECTX));
    writeRingRegister(REG_A2XX_PA_CL_VTE_CNTL, savedVte |
                      A2XX_PA_CL_VTE_CNTL_VPORT_Z_SCALE_ENA | A2XX_PA_CL_VTE_CNTL_VPORT_Z_OFFSET_ENA);
    writeRingPacket0(REG_A2XX_PA_CL_VPORT_ZSCALE, 2);
    writeRing(coordinateWord(1, 0));
    writeRing(0);
    writeRingRegister(REG_A2XX_RB_DEPTHCONTROL, A2XX_RB_DEPTHCONTROL_Z_ENABLE |
                      A2XX_RB_DEPTHCONTROL_Z_WRITE_ENABLE | A2XX_RB_DEPTHCONTROL_ZFUNC(FUNC_LESS));
  } else if (stencil) {
    writeRingRegister(REG_A2XX_RB_DEPTHCONTROL, 0);
  }
  for (UInt32 index = 0; index < _quadLayerCount; ++index) {
    const UInt32 position = vertices + index * kQuadGeometryStride;
    // Stamp the whole mask before narrowing the scissor for the translucent draw.
    if (stencil && index == 1)
      stampQuadStencil(vertices + _quadLayerCount * kQuadGeometryStride, textures,
                       savedColorMask, depth ? FUNC_LESS : FUNC_ALWAYS);
    if (blending)
      setQuadBlendState(index == 1);
    if (scissored && index == 1) {
      const XenosQuadScissor &rect = _quadFrontScissor;
      setGpuWindowScissor(rect.left, rect.top, rect.right, rect.bottom, true);
    }
    drawTextureLayer(position, position + CACHELINE_SIZE, textures + index * PAGE_SIZE);
  }
  // Resolve the full tile so the clipped background remains in the readback.
  if (scissored)
    setGpuWindowScissor(0, 0, kClearSide, kClearSide);
  if (stencil)
    writeRingRegister(REG_A2XX_RB_DEPTHCONTROL, 0);
  if (depth) {
    writeRingRegister(REG_A2XX_PA_CL_CLIP_CNTL, savedClip);
    writeRingRegister(REG_A2XX_PA_CL_VTE_CNTL, savedVte);
    writeRingPacket0(REG_A2XX_PA_CL_VPORT_ZSCALE, 2);
    writeRing(savedZScale);
    writeRing(savedZOffset);
  }
  if (blending)
    setQuadBlendState(false);
  resolveQuadColor(readback);
  flushQuadResolve(readback);
  if (anisotropic) {
    const UInt32 mip = base + kQuadMipOffset;
    const UInt32 striped = base + kQuadAnisoTextureOffset;
    syncGpuMemory(mip, 2 * PAGE_SIZE, kCoherVertexAndTextureCaches);
    clearQuadSampleTarget(base, kSampleClearColor);
    beginQuadSampleDraw();
    for (UInt32 index = 0; index < kAnisoRegionCount; ++index) {
      const UInt32 position = vertices + sampleGeometryOffset + index * kQuadGeometryStride;
      // The two angled draws use identical texture gradients. Only the
      // maximum anisotropy changes, from disabled to 16:1.
      const UInt32 maxAniso = index == 0 ? 0 : kAniso16To1;
      drawTextureLayer(position, position + CACHELINE_SIZE, striped, SQ_TEX_FILTER_BILINEAR, mip,
                       SQ_TEX_FILTER_BILINEAR, 0, maxAniso);
    }
    resolveQuadColor(base + kQuadSampleOffset);
    flushQuadResolve(base + kQuadSampleOffset);
  } else if (mipmap) {
    const UInt32 mip = base + kQuadMipOffset;
    const UInt32 buildPosition = vertices + sampleGeometryOffset;
    drawQuadTexturePass(readback, buildPosition, base, mip, SQ_TEX_FILTER_POINT);

    clearQuadSampleTarget(base, kSampleClearColor);
    beginQuadSampleDraw();
    if (trilinear) {
      // All three draws have the same 1:1 UV gradients. Bias selects
      // LOD 0, 0.5, and 1 with bilinear sampling within each level.
      for (UInt32 index = 0; index < kTrilinearRegionCount; ++index) {
        const UInt32 position = buildPosition + (index + 1) * kQuadGeometryStride;
        drawTextureLayer(position, position + CACHELINE_SIZE, readback, SQ_TEX_FILTER_BILINEAR, mip,
                         SQ_TEX_FILTER_BILINEAR, kTrilinearLodSteps[index]);
      }
    } else {
      // Screen size selects level 1 at 2:1 minification and level 0 at 1:1.
      for (UInt32 position = buildPosition + kQuadGeometryStride;
           position <= buildPosition + 2 * kQuadGeometryStride; position += kQuadGeometryStride)
        drawTextureLayer(position, position + CACHELINE_SIZE, readback, SQ_TEX_FILTER_POINT, mip);
    }
    resolveQuadColor(base + kQuadSampleOffset);
    flushQuadResolve(base + kQuadSampleOffset);
  } else if (renderTexture) {
    drawQuadTexturePass(readback, vertices + sampleGeometryOffset, base, base + kQuadSampleOffset,
                         bilinear ? SQ_TEX_FILTER_BILINEAR : SQ_TEX_FILTER_POINT);
  }

  writeRingPacket0(kXenosCoherDestBase1, 1);
  writeRing(savedCoherDest);
  writeRingPacket0(kXenosColorClear, 2);
  writeRing(savedClear);
  writeRing(savedClearLo);
  if (blending) {
    writeRingPacket0(REG_A2XX_RB_BLEND_CONTROL, 2);
    writeRing(savedBlend);
    writeRing(savedColorControl);
  }
  writeRingPacket0(REG_A2XX_RB_SURFACE_INFO, stencil ? 3 : 2);
  writeRing(ALIGN(_fbWidth, 32));
  writeRing(k_8_8_8_8 << 16);
  if (stencil) {
    writeRing(savedDepthInfo);
    writeRingRegister(kXenosDepthClear, savedDepthClear);
    writeRingRegister(kXenosHiZControl, savedHiZ);
    writeRingRegister(REG_A2XX_RB_DEPTHCONTROL, savedDepthControl);
    writeRingRegister(REG_A2XX_RB_STENCILREFMASK, savedStencilRef);
  }
  restoreQuadDisplayAndFence(fence);
  return true;
}

void XenosController::restoreQuadDisplayAndFence(UInt32 fence) {
  setGpuViewport(_fbWidth, _fbHeight);
  setGpuScissors(_fbWidth, _fbHeight);
  setResolveTarget(_gpuPhysAddr, _fbWidth, _fbHeight, kResolveConvert, kResolveDisplayFormat);

  writeRingPacket3(CP_MEM_WRITE, 2);
  writeRing(fence | kMarkerSwap8In32);
  writeRing(_quadTicket);
  syncRingBuffer();
}

void XenosController::drawTextureLayer(UInt32 positions, UInt32 uv, UInt32 texture,
                                         enum sq_tex_filter filter, UInt32 mipAddress,
                                         enum sq_tex_filter mipFilter, UInt32 lodBiasSteps, UInt32 anisoFilter) {
  setGpuVertices(positions, 12 * sizeof(UInt32), uv, 8 * sizeof(UInt32));
  setGpuTexture(texture, kClearSide, kClearSide, filter, true, mipAddress, mipFilter, lodBiasSteps, anisoFilter);
  writeRingPacket3(CP_DRAW_INDX, 2);
  writeRing(0);
  writeRing((4 << 16) | DRAW(DI_PT_TRISTRIP, DI_SRC_SEL_AUTO_INDEX,
    INDEX_SIZE_IGN, IGNORE_VISIBILITY));
  writeRingPacket0(kXenosWaitUntil, 1);
  writeRing(kWait3dIdleClean);
}

void XenosController::stampQuadStencil(UInt32 vertices, UInt32 texture, UInt32 colorMask,
                                         enum adreno_compare_func frontDepthFunc) {
  // Keep depth writes disabled. Both faces use the front stencil settings.
  const UInt32 depthStencil = A2XX_RB_DEPTHCONTROL_STENCIL_ENABLE | A2XX_RB_DEPTHCONTROL_Z_ENABLE;
  const UInt32 refAndReadMask = A2XX_RB_STENCILREFMASK_STENCILREF(1) |
                                A2XX_RB_STENCILREFMASK_STENCILMASK(0xff);
  writeRingRegister(REG_A2XX_RB_COLOR_MASK, 0);
  writeRingRegister(REG_A2XX_RB_STENCILREFMASK, refAndReadMask |
                                           A2XX_RB_STENCILREFMASK_STENCILWRITEMASK(0xff));
  writeRingRegister(REG_A2XX_RB_DEPTHCONTROL, depthStencil |
                                          A2XX_RB_DEPTHCONTROL_ZFUNC(FUNC_ALWAYS) |
                                          A2XX_RB_DEPTHCONTROL_STENCILFUNC(FUNC_ALWAYS) |
                                          A2XX_RB_DEPTHCONTROL_STENCILZPASS(STENCIL_REPLACE));
  for (UInt32 index = 0; index < kXenosQuadStencilRects; ++index) {
    const UInt32 position = vertices + index * kQuadGeometryStride;
    drawTextureLayer(position, position + CACHELINE_SIZE, texture);
  }

  // The translucent quad reads stencil 1. KEEP is zero for all outcomes.
  writeRingRegister(REG_A2XX_RB_COLOR_MASK, colorMask);
  writeRingRegister(REG_A2XX_RB_STENCILREFMASK, refAndReadMask);
  writeRingRegister(REG_A2XX_RB_DEPTHCONTROL, depthStencil |
                                          A2XX_RB_DEPTHCONTROL_ZFUNC(frontDepthFunc) |
                                          A2XX_RB_DEPTHCONTROL_STENCILFUNC(FUNC_EQUAL));
}

void XenosController::setQuadBlendState(bool enabled) {
  // Use straight-alpha source-over for RGB and alpha.
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

void XenosController::flushQuadResolve(UInt32 address) {
  writeRingPacket3(CP_EVENT_WRITE, 1);
  writeRing(kCacheFlushEvent);
  writeRingRegister(kXenosCoherDestBase1, 0);
  syncGpuMemory(address, PAGE_SIZE, kCoherResolveActions);
  writeRingPacket0(kXenosWaitUntil, 1);
  writeRing(kWait3dIdleClean);
}

void XenosController::clearQuadSampleTarget(UInt32 discard, UInt32 clearColor) {
  writeRingPacket0(REG_A2XX_RB_SURFACE_INFO, 2);
  writeRing(kClearEdRamPitch);
  writeRing((k_8_8_8_8 << 16) | kSampleTile);
  setGpuEdRamMode(EDRAM_COPY);
  writeRingPacket0(kXenosColorClear, 2);
  writeRing(clearColor);
  writeRing(clearColor);
  setResolveTarget(discard, kClearSide, kClearSide, kResolveConvert | kResolveColorClear,
                   kResolveDisplayFormat | kMarkerSwap8In32);
  drawResolveRectangle();
  writeRingPacket0(kXenosWaitUntil, 1);
  writeRing(kWait3dIdleClean);
}

void XenosController::beginQuadSampleDraw() {
  writeRingRegister(REG_A2XX_RB_COPY_CONTROL, 0);
  writeRingPacket3(CP_INVALIDATE_STATE, 1);
  writeRing(0x00000200);
  setGpuTexturePixelShader(true);
  setGpuEdRamMode(COLOR_DEPTH);
}

void XenosController::drawQuadTexturePass(UInt32 texture, UInt32 positions, UInt32 discard,
                                            UInt32 result, enum sq_tex_filter filter) {
  clearQuadSampleTarget(discard, kSampleClearColor);
  beginQuadSampleDraw();
  drawTextureLayer(positions, positions + CACHELINE_SIZE, texture, filter);
  resolveQuadColor(result);
  flushQuadResolve(result);
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
