from pathlib import Path
root = Path(__file__).resolve().parent
kext = root / 'source/XenonCoreGraphics'
src = kext / 'src'

def change(path, substitutions):
    text = path.read_text()
    for old, new in substitutions:
        assert text.count(old) == 1, (path, old, text.count(old))
        text = text.replace(old, new)
    path.write_text(text)

change(kext / 'Makefile', [('0.0.5d36', '0.0.5d37')])
change(src / 'XenosQuadProtocol.h', [
    ('  kXenosDrawableImport = 22,', '''  kXenosDrawableImport = 22,
  kXenosSurfaceEnqueue = 23,
  kXenosSurfaceQueueStatus = 24,'''),
    ('typedef struct {\n  uint32_t words[kXenosDrawableTokenWords];', '''enum {
  kXenosSurfaceQueueWaiting = 1,
  kXenosSurfaceQueueInFlight = 2,
  kXenosSurfaceQueueComplete = 3,
  kXenosSurfaceQueueCancelled = 4,
  kXenosSurfaceQueueFailed = 5,
};

typedef struct {
  uint32_t handle, state, fence, completion, ownerClosed;
  uint32_t liveSurfaces, pending, activeTicket;
} XenosSurfaceQueueInfo;

typedef struct {
  uint32_t words[kXenosDrawableTokenWords];'''),
])
change(src / 'XenosQuadClient.hpp', [
    ('  IOReturn surfaceAction(', '''  IOReturn enqueueSurface(void *handle, void *argb, void *ticket, void *ahead, void *, void *);
  IOReturn surfaceQueueStatus(void *input, void *output, void *inputSize, void *outputSize, void *, void *);
  IOReturn surfaceAction('''),
])
change(src / 'XenosQuadClient.cpp', [
    ('    {NULL, (IOMethod)&XenosQuadClient::importDrawable, kIOUCScalarIScalarO, kXenosDrawableTokenWords, 0},',
     '''    {NULL, (IOMethod)&XenosQuadClient::importDrawable, kIOUCScalarIScalarO, kXenosDrawableTokenWords, 0},
    {NULL, (IOMethod)&XenosQuadClient::enqueueSurface, kIOUCScalarIScalarO, 2, 2},
    {NULL, (IOMethod)&XenosQuadClient::surfaceQueueStatus, kIOUCStructIStructO,
     sizeof(UInt32), sizeof(XenosSurfaceQueueInfo)},'''),
    ('IOReturn XenosQuadClient::sendUpload(', '''IOReturn XenosQuadClient::enqueueSurface(void *handle, void *argb, void *ticket, void *ahead,
                                         void *, void *) {
  UInt32 values[2];
  const IOReturn result = surfaceAction(&XenosController::enqueueSurfaceClear, handle, argb, values);
  if (result == kIOReturnSuccess) {
    *static_cast<UInt32 *>(ticket) = values[0];
    *static_cast<UInt32 *>(ahead) = values[1];
  }
  return result;
}

IOReturn XenosQuadClient::surfaceQueueStatus(void *input, void *output, void *inputSize,
                                             void *outputSize, void *, void *) {
  if (!input || !output || !outputSize || reinterpret_cast<uintptr_t>(inputSize) != sizeof(UInt32) ||
      *static_cast<IOByteCount *>(outputSize) != sizeof(XenosSurfaceQueueInfo))
    return kIOReturnBadArgument;
  return surfaceAction(&XenosController::readSurfaceQueue, input, output, NULL);
}

IOReturn XenosQuadClient::sendUpload('''),
])
change(src / 'XenosController.hpp', [
    ('  UInt32                     _quadTicket;', '''  UInt32                     _quadTicket;
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
  UInt32                     _surfaceCompletion;'''),
    ('  void revokeDrawableGrant();', '''  void revokeDrawableGrant();
  static IOReturn enqueueSurfaceClear(OSObject *owner, void *client, void *handle, void *argb, void *output);
  static IOReturn readSurfaceQueue(OSObject *owner, void *client, void *input, void *output, void *);
  SurfaceQueueRecord *surfaceQueueRecord(UInt32 ticket);
  UInt32 surfaceQueueIndex(XenosQuadClient *client) const;
  SurfaceQueueEntry removeSurfaceQueueEntry(UInt32 index);
  void closeQueuedSurface(XenosQuadClient *client);
  void startNextSurface();
  void retireSurfaceQueueEntry(UInt32 fence);'''),
    ('  void startQuad(XenosQuadClient *client, UInt32 texture, bool uploaded,\n                 UInt32 geometryMode, UInt32 layers, UInt32 *output);',
     '''  UInt32 reserveQuadTicket();
  void startQuad(XenosQuadClient *client, UInt32 texture, bool uploaded,
                 UInt32 geometryMode, UInt32 layers, UInt32 *output, UInt32 reservedTicket = 0);'''),
])
controller = src / 'XenosController.cpp'
change(controller, [
    ('  _quadTicket = _quadTexture = _quadWait = _quadCooldown = 0;',
     '''  _quadTicket = _nextQuadTicket = _quadTexture = _quadWait = _quadCooldown = 0;
  _surfaceQueueCount = _surfaceActiveTicket = _surfaceCompletion = 0;
  bzero(_surfaceRecords, sizeof(_surfaceRecords));'''),
    ('  return _quadClient || _quadScratch || _quadCooldown ? kIOReturnBusy : kIOReturnSuccess;',
     '  return _quadClient || _quadScratch || _quadCooldown || _surfaceQueueCount\n      ? kIOReturnBusy : kIOReturnSuccess;'),
    ('''void XenosController::startQuad(XenosQuadClient *client, UInt32 texture, bool uploaded,
                                UInt32 geometryMode, UInt32 layers, UInt32 *output) {
  if (++_quadTicket == 0)
    ++_quadTicket;''',
     '''UInt32 XenosController::reserveQuadTicket() {
  if (++_nextQuadTicket == 0)
    ++_nextQuadTicket;
  return _nextQuadTicket;
}

void XenosController::startQuad(XenosQuadClient *client, UInt32 texture, bool uploaded,
                                UInt32 geometryMode, UInt32 layers, UInt32 *output, UInt32 reservedTicket) {
  _quadTicket = reservedTicket ? reservedTicket : reserveQuadTicket();'''),
    ('''  if (!_quadClient && !_quadScratch)
    return true;''',
     '''  if (!_quadClient && !_quadScratch) {
    if (!_surfaceQueueCount || _quadCooldown)
      return true;
    startNextSurface();
  }'''),
    ('''        finishQuad(kXenosQuadUnavailable, 0, 0, 0);
        return true;''',
     '''        finishQuad(kXenosQuadUnavailable, 0, 0, 0);
        retireSurfaceQueueEntry(0);
        return true;'''),
    ('''      finishQuad(kXenosQuadUnavailable, 0, 0, 0);
      return true;
    }
    _quadWait = 0;''',
     '''      finishQuad(kXenosQuadUnavailable, 0, 0, 0);
      retireSurfaceQueueEntry(0);
      return true;
    }
    if (SurfaceQueueRecord *record = surfaceQueueRecord(_surfaceActiveTicket))
      record->state = kXenosSurfaceQueueInFlight;
    _quadWait = 0;'''),
    ('''    _surfaceJobPixels = NULL;
  }
  return true;''',
     '''    _surfaceJobPixels = NULL;
  }
  retireSurfaceQueueEntry(fence);
  return true;'''),
    ('''  _quadClient = NULL;
  _quadCooldown = kQuadCooldownTicks;
  client->release();''',
     '''  _quadClient = NULL;
  _quadCooldown = kQuadCooldownTicks;
  if (state != kXenosQuadPassed) {
    if (SurfaceQueueRecord *record = surfaceQueueRecord(_surfaceActiveTicket))
      record->state = kXenosSurfaceQueueFailed;
  }
  client->release();'''),
    ('''  if (self->_quadClient == client && self->_quadGeometryMode == kXenosSurfaceClear)
    return kIOReturnBusy;''',
     '''  if ((self->_quadClient == client && self->_quadGeometryMode == kXenosSurfaceClear) ||
      self->surfaceQueueIndex(client) != self->_surfaceQueueCount)
    return kIOReturnBusy;'''),
    ('''  self->dropSurface(client);
  if (client->_drawableView)''',
     '''  self->closeQueuedSurface(client);
  self->dropSurface(client);
  if (client->_drawableView)'''),
    ('bool XenosController::submitQuad() {',
     (root / 'queue-controller.inc').read_text() + '\nbool XenosController::submitQuad() {'),
])
