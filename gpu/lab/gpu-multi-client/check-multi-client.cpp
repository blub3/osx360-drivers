#include "XenosQuadProtocol.h"
#include <assert.h>
#include <initializer_list>
#include <stdio.h>
#include <string.h>
using UInt32 = uint32_t;
using IOReturn = int;
enum {
    kIOReturnSuccess, kIOReturnBadArgument, kIOReturnBusy, kIOReturnNotReady, kIOReturnNotFound
};
struct OSObject {
    bool inactive = false;
    unsigned refs = 1;
    bool isInactive() const { return inactive; }
    void retain() { ++refs; }
    void release() { assert(refs); --refs; }
};
struct XenosQuadClient : OSObject {
    OSObject *_surface;
    UInt32 _surfaceHandle, _surfaceTicket, _ticket;
    UInt32 _result[kXenosQuadUploadResultWords];
    bool _surfaceReady;
};
static void IOLog(const char *, ...) {}
static void *scalar(UInt32 value) {
    return reinterpret_cast<void *>(static_cast<uintptr_t>(value));
}
struct XenosController : OSObject {
    XenosQuadClient *_quadClient;
    OSObject *_quadScratch;
    UInt32 _quadTicket, _surfaceColor, _surfaceCount;
  UInt32                     _nextQuadTicket;
  enum { kSurfaceQueueCapacity = 4, kSurfaceQueueHistory = 16 };
  struct SurfaceQueueEntry {
    XenosQuadClient *client;
    UInt32 ticket, argb;
  };
  struct SurfaceQueueRecord {
    UInt32 ticket, handle, state, fence, completion, ownerClosed;
  };
  SurfaceQueueEntry          _surfaceQueue[kSurfaceQueueCapacity];
  SurfaceQueueRecord         _surfaceRecords[kSurfaceQueueHistory];
  UInt32                     _surfaceQueueCount;
  UInt32                     _surfaceActiveTicket;
  UInt32                     _surfaceCompletion;
  static IOReturn enqueueSurfaceClear(OSObject *owner, void *client, void *handle, void *argb, void *output);
  static IOReturn readSurfaceQueue(OSObject *owner, void *client, void *input, void *output, void *);
  SurfaceQueueRecord *surfaceQueueRecord(UInt32 ticket);
  UInt32 surfaceQueueIndex(XenosQuadClient *client) const;
  SurfaceQueueEntry removeSurfaceQueueEntry(UInt32 index);
  void closeQueuedSurface(XenosQuadClient *client);
  void startNextSurface();
  void retireSurfaceQueueEntry(UInt32 fence);
  void restoreQuadDisplayAndFence(UInt32 fence);

    bool ownsSurface(XenosQuadClient *client, UInt32 handle) const {
        return client->_surface && handle == client->_surfaceHandle;
    }
    UInt32 reserveQuadTicket();
    void startQuad(XenosQuadClient *client, UInt32, bool, UInt32 mode, UInt32,
                   UInt32 *out, UInt32 ticket) {
        assert(mode == kXenosSurfaceClear && !_quadClient && !_quadScratch);
        _quadClient = client;
        client->retain();
        *out = _quadTicket = ticket;
    }
    void finishQuad(UInt32 state, UInt32, UInt32, UInt32) {
        if (state != kXenosQuadPassed) {
            if (auto *record = surfaceQueueRecord(_surfaceActiveTicket))
                record->state = kXenosSurfaceQueueFailed;
        }
        _quadClient->release();
        _quadClient = nullptr;
    }
    UInt32 submit(XenosQuadClient &client, UInt32 expectedAhead) {
        UInt32 values[2] = {};
        assert(enqueueSurfaceClear(this, &client, scalar(client._surfaceHandle),
                                  scalar(0xff123456), values) == kIOReturnSuccess);
        assert(values[1] == expectedAhead && values[0]);
        return values[0];
    }
    void beginGpu() {
        _quadScratch = _quadClient->_surface;
        _quadScratch->retain();
        surfaceQueueRecord(_surfaceActiveTicket)->state = kXenosSurfaceQueueInFlight;
    }
    void gpuComplete() {
        finishQuad(kXenosQuadPassed, _quadTicket, 1024, 0);
        _quadScratch->release();
        _quadScratch = nullptr;
        retireSurfaceQueueEntry(_quadTicket);
    }
    void closeClient(XenosQuadClient &client) {
        closeQueuedSurface(&client);
        client._surface->release();
        client._surface = nullptr;
        client._surfaceHandle = 0;
        --_surfaceCount;
    }
};
UInt32 XenosController::reserveQuadTicket() {
  if (++_nextQuadTicket == 0)
    ++_nextQuadTicket;
  return _nextQuadTicket;
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


int main() {
    XenosController device = {};
    XenosQuadClient a = {}, b = {};
    OSObject aPixels, bPixels;
    a._surface = &aPixels;
    a._surfaceHandle = 21;
    b._surface = &bPixels;
    b._surfaceHandle = 22;
    device._surfaceCount = 2;
    const UInt32 a1 = device.submit(a, 0);
    const UInt32 b1 = device.submit(b, 1);
    assert(device._surfaceQueueCount == 2 && a.refs == 2 && b.refs == 2);
    UInt32 rejected[2] = {};
    assert(device.enqueueSurfaceClear(&device, &a, scalar(21), scalar(0xff123456),
                                     rejected) == kIOReturnBusy && !rejected[0]);
    for (UInt32 ticket : {a1, b1}) {
        device.startNextSurface();
        assert(device._quadTicket == ticket);
        device.beginGpu();
        device.gpuComplete();
    }
    XenosSurfaceQueueInfo info;
    assert(device.readSurfaceQueue(&device, &a, const_cast<UInt32 *>(&a1), &info, nullptr) == kIOReturnSuccess);
    assert(info.fence == a1 && info.state == kXenosSurfaceQueueComplete && info.completion == 1);
    auto *pair = device.surfaceQueueRecord(b1);
    assert(pair->fence == b1 && pair->completion == 2 && device._surfaceCount == 2);
    const UInt32 a2 = device.submit(a, 0);
    const UInt32 b2 = device.submit(b, 1);
    device.startNextSurface();
    assert(device._quadTicket == a2);
    device.beginGpu();
    device.closeClient(b);
    auto *cancelled = device.surfaceQueueRecord(b2);
    assert(cancelled->state == kXenosSurfaceQueueCancelled &&
           !cancelled->fence && cancelled->ownerClosed && !bPixels.refs);
    assert(device._surfaceCount == 1 && device._surfaceQueueCount == 0);
    device.gpuComplete();

    OSObject reopenedPixels;
    b._surface = &reopenedPixels;
    b._surfaceHandle = 23;
    ++device._surfaceCount;
    const UInt32 b3 = device.submit(b, 0);
    device.startNextSurface();
    device.beginGpu();
    device.closeClient(b);
    auto *departed = device.surfaceQueueRecord(b3);
    assert(departed->ownerClosed && departed->state == kXenosSurfaceQueueInFlight);
    assert(reopenedPixels.refs == 1 && device._surfaceActiveTicket == b3);
    const UInt32 a3 = device.submit(a, 1);
    device.gpuComplete();
    assert(departed->fence == b3 && departed->state == kXenosSurfaceQueueComplete &&
           departed->completion == 4 && !reopenedPixels.refs && !device._surfaceActiveTicket);
    device.startNextSurface();
    assert(device._quadTicket == a3);
    device.beginGpu();
    device.gpuComplete();
    assert(device.surfaceQueueRecord(a3)->completion == 5);
    device.closeClient(a);
    assert(!device._surfaceCount && !device._surfaceQueueCount && !device._surfaceActiveTicket);
    puts("FIFO, per-client admission, pending cancellation, in-flight retention and survivor completion passed.");
}
