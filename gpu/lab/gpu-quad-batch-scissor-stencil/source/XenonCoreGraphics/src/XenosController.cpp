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
static const UInt32 kQuadRingDwords = 320;
static const UInt32 kQuadPagesBeforeTextures = 4;
static const UInt32 kQuadGeometryStride = 2 * CACHELINE_SIZE;
static const UInt32 kQuadReadbackOffset = PAGE_SIZE;
static const UInt32 kQuadFenceOffset = 2 * PAGE_SIZE;
static const UInt32 kQuadGeometryOffset = 3 * PAGE_SIZE;
static const UInt32 kQuadTextureOffset = 4 * PAGE_SIZE;
static const UInt32 kClearSide = kXenosQuadTargetSide;
static const UInt32 kClearEdRamPitch = 160; // Two 80-pixel eDRAM tiles per row.
static const UInt32 kClearPixels = kXenosQuadReadbackPixels;
static const UInt32 kClearTile = 0x7f0;
static const UInt32 kStencilTile = 0x7f8; // Separate eDRAM tiles, format 0 is D24S8.
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

IOReturn XenosController::enqueueQuadBatch(XenosQuadClient *client, const XenosQuadBatchUpload &batch,
                                            const UInt8 *frontAlpha, UInt32 mode,
                                            const XenosQuadScissor *frontMask,
                                            const XenosQuadScissor *frontScissor,
                                            UInt32 *output) {
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
  startQuad(client, 0, true, mode, kXenosQuadBatchLayers, output);
  return kIOReturnSuccess;
}

IOReturn XenosController::quadQueueReady(XenosQuadClient *client) {
  if (isInactive() || client->isInactive())
    return kIOReturnNotReady;
  return _quadClient || _quadScratch || _quadCooldown ? kIOReturnBusy : kIOReturnSuccess;
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

void XenosController::startQuad(XenosQuadClient *client, UInt32 texture, bool uploaded,
                                UInt32 geometryMode, UInt32 layers, UInt32 *output) {
  if (++_quadTicket == 0)
    ++_quadTicket;
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
  _quadWait = 0;
  client->_ticket = _quadTicket;
  bzero(client->_result, sizeof(client->_result));
  *output = _quadTicket;

  setProperty("XenosUserQuadTicket", _quadTicket, 32);
  const char *source = geometryMode == kXenosQuadUploadScissorStencilBatch ? "Scissor and Stencil Batch" :
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
         _quadGeometryMode == kXenosQuadUploadScissorStencilBatch;
}

bool XenosController::quadUsesStencil() const {
  return _quadGeometryMode == kXenosQuadUploadStencilBatch ||
         _quadGeometryMode == kXenosQuadUploadScissorStencilBatch;
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
    UInt32 hash = XENOS_QUAD_HASH_INITIAL;
    const bool blending = quadUsesBlending();
    const bool scissored = quadUsesScissor();
    const bool stenciled = quadUsesStencil();
    const bool combined = scissored && stenciled;
    const char *blendLog = combined ? "XenosBatchScissorStencil" :
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
        const bool second = frontGeometry && !clipped;
        UInt32 expected = destination;
        bool translucent = false;
        if (second) {
          const UInt32 source = quadLayerTexel(1, u, v);
          expected = blending ? quadSourceOver(source, destination) : source;
          const UInt32 alpha = source >> 24;
          translucent = blending && alpha > 0 && alpha < 255;
        }
        const UInt32 observed = pixels[resolveTileWord(x, y)];
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
        if (combined && matched && clipped) {
          _quadScissorRejectOnly += scissorReject && !stencilReject;
          _quadStencilRejectOnly += stencilReject && !scissorReject;
          _quadBothReject += scissorReject && stencilReject;
        }
        if (first || second) {
          hash = xenosQuadHashByte(hash, (observed >> 16) & 0xff);
          hash = xenosQuadHashByte(hash, (observed >> 8) & 0xff);
          hash = xenosQuadHashByte(hash, observed & 0xff);
        }
      }
    }
    finishQuad(mismatches ? kXenosQuadPixelsFailed : kXenosQuadPassed,
               fence, kClearPixels, mismatches, mismatches ? 0 : hash);
  }
  _quadScratch->release();
  _quadScratch = NULL;
  return true;
}

void XenosController::finishQuad(UInt32 state, UInt32 fence, UInt32 checked,
                                  UInt32 mismatches, UInt32 hash) {
  XenosQuadClient *client = _quadClient;
  client->_result[0] = state;
  client->_result[1] = fence;
  client->_result[2] = checked;
  client->_result[3] = mismatches;
  client->_result[4] = hash;
  _quadClient = NULL;
  _quadCooldown = kQuadCooldownTicks;
  client->release();

  const char *result = state == kXenosQuadPassed ? "PASS" :
                       state == kXenosQuadPixelsFailed ? "FAIL pixels" :
                       state == kXenosQuadTimedOut ? "FAIL timeout" : "FAIL unavailable";
  setProperty("XenosUserQuadFence", fence, 32);
  setProperty("XenosUserQuadPixelsChecked", checked, 32);
  setProperty("XenosUserQuadMismatches", mismatches, 32);
  setProperty("XenosUserQuadReadbackHash", hash, 32);
  setProperty("XenosUserQuadResult", result);
  if (_quadGeometryMode == kXenosQuadUploadScissorStencilBatch) {
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

bool XenosController::submitQuad() {
  // Use spare eDRAM tiles beyond the largest supported desktop surface.
  if (!_fbWidth || !_fbHeight || _fbWidth > 1920 || _fbHeight > 1080)
    return false;

  const UInt32 scratchBytes = (kQuadPagesBeforeTextures + _quadLayerCount) * PAGE_SIZE;
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
  for (UInt32 index = 0; index < _quadLayerCount; ++index) {
    const QuadLayer &layer = _quadLayers[index];
    UInt8 *geometry = memory + kQuadGeometryOffset + index * kQuadGeometryStride;
    packQuadGeometry(layer.payload.geometry, reinterpret_cast<UInt32 *>(geometry),
                     reinterpret_cast<UInt32 *>(geometry + CACHELINE_SIZE), layer.rotated);

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
  syncData(memory + kQuadGeometryOffset, geometryCount * kQuadGeometryStride);
  syncData(memory + kQuadTextureOffset, _quadLayerCount * PAGE_SIZE);
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
  if (stencil)
    writeRingRegister(REG_A2XX_RB_DEPTHCONTROL, 0);
  for (UInt32 index = 0; index < _quadLayerCount; ++index) {
    const UInt32 position = vertices + index * kQuadGeometryStride;
    // Stamp the whole mask before narrowing the scissor for the translucent draw.
    if (stencil && index == 1)
      stampQuadStencil(vertices + _quadLayerCount * kQuadGeometryStride, textures, savedColorMask);
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
  if (blending)
    setQuadBlendState(false);
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
  setGpuVertices(positions, 12 * sizeof(UInt32), uv, 8 * sizeof(UInt32));
  setGpuTexture(texture, kClearSide, kClearSide, SQ_TEX_FILTER_POINT, true);
  writeRingPacket3(CP_DRAW_INDX, 2);
  writeRing(0);
  writeRing((4 << 16) | DRAW(DI_PT_TRISTRIP, DI_SRC_SEL_AUTO_INDEX,
    INDEX_SIZE_IGN, IGNORE_VISIBILITY));
  writeRingPacket0(kXenosWaitUntil, 1);
  writeRing(kWait3dIdleClean);
}

void XenosController::stampQuadStencil(UInt32 vertices, UInt32 texture, UInt32 colorMask) {
  // Keep depth writes disabled. Both faces use the front stencil settings.
  const UInt32 depthAlways = A2XX_RB_DEPTHCONTROL_STENCIL_ENABLE |
                             A2XX_RB_DEPTHCONTROL_Z_ENABLE |
                             A2XX_RB_DEPTHCONTROL_ZFUNC(FUNC_ALWAYS);
  const UInt32 refAndReadMask = A2XX_RB_STENCILREFMASK_STENCILREF(1) |
                                A2XX_RB_STENCILREFMASK_STENCILMASK(0xff);
  writeRingRegister(REG_A2XX_RB_COLOR_MASK, 0);
  writeRingRegister(REG_A2XX_RB_STENCILREFMASK, refAndReadMask |
                                           A2XX_RB_STENCILREFMASK_STENCILWRITEMASK(0xff));
  writeRingRegister(REG_A2XX_RB_DEPTHCONTROL, depthAlways |
                                          A2XX_RB_DEPTHCONTROL_STENCILFUNC(FUNC_ALWAYS) |
                                          A2XX_RB_DEPTHCONTROL_STENCILZPASS(STENCIL_REPLACE));
  for (UInt32 index = 0; index < kXenosQuadStencilRects; ++index) {
    const UInt32 position = vertices + index * kQuadGeometryStride;
    drawTextureLayer(position, position + CACHELINE_SIZE, texture);
  }

  // The translucent quad reads stencil 1. KEEP is zero for all outcomes.
  writeRingRegister(REG_A2XX_RB_COLOR_MASK, colorMask);
  writeRingRegister(REG_A2XX_RB_STENCILREFMASK, refAndReadMask);
  writeRingRegister(REG_A2XX_RB_DEPTHCONTROL, depthAlways |
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
