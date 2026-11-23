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
