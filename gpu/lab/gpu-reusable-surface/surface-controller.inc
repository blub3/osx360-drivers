bool XenosController::ownsSurface(XenosQuadClient *client, UInt32 handle) const {
  return handle && client->_surface && handle == client->_surfaceHandle;
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

  IOBufferMemoryDescriptor *surface = IOBufferMemoryDescriptor::withOptions(
      kIOMemoryPhysicallyContiguous, kSurfaceStorageBytes, PAGE_SIZE);
  if (!surface)
    return kIOReturnNoMemory;
  IOByteCount length = 0;
  const IOPhysicalAddress physical = surface->getPhysicalSegment(0, &length);
  if (!surface->getBytesNoCopy() || !physical || length < kSurfaceStorageBytes ||
      (physical & (PAGE_SIZE - 1)) || physical > kDisplayMemoryStart - kSurfaceStorageBytes) {
    surface->release();
    return kIOReturnNoResources;
  }

  if (++self->_surfaceSerial == 0)
    ++self->_surfaceSerial;
  ++self->_surfaceCount;
  client->_surface = surface;
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

  const UInt8 *memory = static_cast<const UInt8 *>(client->_surface->getBytesNoCopy());
  const volatile UInt32 *tiled = reinterpret_cast<const volatile UInt32 *>(memory + kQuadReadbackOffset);
  XenosSurfaceReadback &readback = *static_cast<XenosSurfaceReadback *>(output);
  for (UInt32 row = 0; row < kXenosSurfaceReadRows; ++row)
    for (UInt32 x = 0; x < kXenosSurfaceSide; ++x)
      readback.argb[row * kXenosSurfaceSide + x] = tiled[resolveTileWord(x, request.firstRow + row)];
  return kIOReturnSuccess;
}

void XenosController::dropSurface(XenosQuadClient *client) {
  if (client->_surface) {
    client->_surface->release();
    client->_surface = NULL;
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
  if (self->_quadClient == client && self->_quadGeometryMode == kXenosSurfaceClear)
    return kIOReturnBusy;
  self->dropSurface(client);
  return kIOReturnSuccess;
}

IOReturn XenosController::closeClientSurface(OSObject *owner, void *clientArg, void *, void *, void *) {
  static_cast<XenosController *>(owner)->dropSurface(static_cast<XenosQuadClient *>(clientArg));
  return kIOReturnSuccess;
}

bool XenosController::submitSurfaceClear() {
  if (!_quadClient->_surface || !_fbWidth || !_fbHeight || _fbWidth > 1920 || _fbHeight > 1080)
    return false;

  // The work loop keeps a separate reference until the fence lands, including
  // when the user closes a connection or gives up waiting.
  _quadScratch = _quadClient->_surface;
  _quadScratch->retain();
  const UInt32 physical = static_cast<UInt32>(_quadScratch->getPhysicalSegment(0, NULL));
  const UInt32 readback = physical + kQuadReadbackOffset;
  UInt8 *memory = static_cast<UInt8 *>(_quadScratch->getBytesNoCopy());
  prepareQuadReadback(memory + kQuadReadbackOffset);
  volatile UInt32 *marker = reinterpret_cast<volatile UInt32 *>(memory + kQuadFenceOffset);
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
  restoreQuadDisplayAndFence(physical + kQuadFenceOffset);
  return true;
}

void XenosController::verifySurfaceClear(UInt8 *memory, UInt32 fence) {
  volatile UInt32 *pixels = reinterpret_cast<volatile UInt32 *>(memory + kQuadReadbackOffset);
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
