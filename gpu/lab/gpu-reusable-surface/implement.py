from pathlib import Path

root = Path(__file__).resolve().parent
kext = root / 'source/XenonCoreGraphics'
src = kext / 'src'

def change(path, replacements):
    text = path.read_text()
    for old, new in replacements:
        assert text.count(old) == 1, (path, old, text.count(old))
        text = text.replace(old, new)
    path.write_text(text)

change(kext / 'Makefile', [('0.0.5d34', '0.0.5d35')])
change(src / 'XenosQuadProtocol.h', [
    ('  kXenosQuadUploadAnisotropicBatch = 16,', '''  kXenosQuadUploadAnisotropicBatch = 16,
  kXenosSurfaceCreate = 17,
  kXenosSurfaceClear = 18,
  kXenosSurfaceRead = 19,
  kXenosSurfaceRelease = 20,'''),
    ('  kXenosQuadMipmapReadbackPixels = 3 * kXenosQuadReadbackPixels,',
     '''  kXenosQuadMipmapReadbackPixels = 3 * kXenosQuadReadbackPixels,
  kXenosSurfaceSide = kXenosQuadTargetSide,
  kXenosSurfaceReadRows = 8,
  kXenosSurfaceReadPixels = kXenosSurfaceSide * kXenosSurfaceReadRows,'''),
    ('// UVs name texel edges.', '''// Read a band of 8 rows in row-major opaque ARGB8888. A clear's ticket
// selects the contents so a later clear cannot silently replace the readback.
typedef struct {
  uint32_t handle;
  uint32_t ticket;
  uint32_t firstRow;
} XenosSurfaceReadRequest;

typedef struct {
  uint32_t argb[kXenosSurfaceReadPixels];
} XenosSurfaceReadback;

// UVs name texel edges.'''),
])
change(src / 'XenosQuadClient.hpp', [
    ('\nclass XenosController;', '\nclass IOBufferMemoryDescriptor;\nclass XenosController;'),
    ('  bool start(IOService *provider);', '''  bool start(IOService *provider);
  void stop(IOService *provider);
  void free();'''),
    ('  IOReturn sendUpload(', '''  IOReturn createSurface(void *width, void *height, void *handle, void *, void *, void *);
  IOReturn clearSurface(void *handle, void *argb, void *ticket, void *, void *, void *);
  IOReturn readSurface(void *input, void *output, void *inputSize, void *outputSize, void *, void *);
  IOReturn releaseSurface(void *handle, void *, void *, void *, void *, void *);
  IOReturn surfaceAction(IOCommandGate::Action action, void *arg0, void *arg1, void *arg2);
  IOReturn sendUpload('''),
    ('  UInt32 _result[kXenosQuadUploadResultWords];', '''  UInt32 _result[kXenosQuadUploadResultWords];
  IOBufferMemoryDescriptor *_surface;
  UInt32 _surfaceHandle;
  UInt32 _surfaceTicket;
  bool _surfaceReady;'''),
])
change(src / 'XenosQuadClient.cpp', [
    ('''  return task && type == 0 && IOUserClient::initWithTask(task, securityToken, type, properties);''',
     '''  _surface = NULL;
  _surfaceHandle = _surfaceTicket = 0;
  _surfaceReady = false;
  return task && type == 0 && IOUserClient::initWithTask(task, securityToken, type, properties);'''),
    ('IOReturn XenosQuadClient::clientClose() {', '''void XenosQuadClient::stop(IOService *provider) {
  XenosController *controller = OSDynamicCast(XenosController, provider);
  if (controller && controller->_quadGate)
    controller->_quadGate->runAction(&XenosController::closeClientSurface, this);
  IOUserClient::stop(provider);
}

void XenosQuadClient::free() {
  if (_surface)
    _surface->release();
  IOUserClient::free();
}

IOReturn XenosQuadClient::clientClose() {'''),
    ('''     sizeof(XenosQuadDepthBatchUpload), sizeof(UInt32)},
  };''',
     '''     sizeof(XenosQuadDepthBatchUpload), sizeof(UInt32)},
    {NULL, (IOMethod)&XenosQuadClient::createSurface, kIOUCScalarIScalarO, 2, 1},
    {NULL, (IOMethod)&XenosQuadClient::clearSurface, kIOUCScalarIScalarO, 2, 1},
    {NULL, (IOMethod)&XenosQuadClient::readSurface, kIOUCStructIStructO,
     sizeof(XenosSurfaceReadRequest), sizeof(XenosSurfaceReadback)},
    {NULL, (IOMethod)&XenosQuadClient::releaseSurface, kIOUCScalarIScalarO, 1, 0},
  };'''),
    ('IOReturn XenosQuadClient::sendUpload(', '''IOReturn XenosQuadClient::surfaceAction(IOCommandGate::Action action, void *arg0,
                                        void *arg1, void *arg2) {
  XenosController *controller = OSDynamicCast(XenosController, getProvider());
  if (!controller || isInactive())
    return kIOReturnNotReady;
  return controller->_quadGate->runAction(action, this, arg0, arg1, arg2);
}

IOReturn XenosQuadClient::createSurface(void *width, void *height, void *handle,
                                        void *, void *, void *) {
  return surfaceAction(&XenosController::createClientSurface, width, height, handle);
}

IOReturn XenosQuadClient::clearSurface(void *handle, void *argb, void *ticket,
                                       void *, void *, void *) {
  return surfaceAction(&XenosController::queueSurfaceClear, handle, argb, ticket);
}

IOReturn XenosQuadClient::readSurface(void *input, void *output, void *inputSize,
                                      void *outputSize, void *, void *) {
  if (!input || !output || !outputSize ||
      reinterpret_cast<uintptr_t>(inputSize) != sizeof(XenosSurfaceReadRequest) ||
      *static_cast<IOByteCount *>(outputSize) != sizeof(XenosSurfaceReadback))
    return kIOReturnBadArgument;
  return surfaceAction(&XenosController::readClientSurface, input, output, NULL);
}

IOReturn XenosQuadClient::releaseSurface(void *handle, void *, void *, void *, void *, void *) {
  return surfaceAction(&XenosController::releaseClientSurface, handle, NULL, NULL);
}

IOReturn XenosQuadClient::sendUpload('''),
])
change(src / 'XenosController.hpp', [
    ('  UInt32                     _quadTicket;', '''  UInt32                     _quadTicket;
  UInt32                     _surfaceSerial;
  UInt32                     _surfaceCount;
  UInt32                     _surfaceColor;'''),
    ('  IOReturn enqueueQuadDepthBatch(', '''  static IOReturn createClientSurface(OSObject *owner, void *client, void *width, void *height, void *handle);
  static IOReturn queueSurfaceClear(OSObject *owner, void *client, void *handle, void *argb, void *ticket);
  static IOReturn readClientSurface(OSObject *owner, void *client, void *input, void *output, void *);
  static IOReturn releaseClientSurface(OSObject *owner, void *client, void *handle, void *, void *);
  static IOReturn closeClientSurface(OSObject *owner, void *client, void *, void *, void *);
  void dropSurface(XenosQuadClient *client);
  bool ownsSurface(XenosQuadClient *client, UInt32 handle) const;
  bool submitSurfaceClear();
  void verifySurfaceClear(UInt8 *memory, UInt32 fence);
  void restoreQuadDisplayAndFence(UInt32 fence);
  IOReturn enqueueQuadDepthBatch('''),
    ('  void clearQuadSampleTarget(UInt32 discard);', '  void clearQuadSampleTarget(UInt32 discard, UInt32 clearColor);'),
])
controller = src / 'XenosController.cpp'
change(controller, [
    ('static const UInt32 kQuadFenceOffset = 2 * PAGE_SIZE;', '''static const UInt32 kQuadFenceOffset = 2 * PAGE_SIZE;
static const UInt32 kSurfaceStorageBytes = kQuadFenceOffset + PAGE_SIZE;
static const UInt32 kSurfaceAllocationLimit = 8;'''),
    ('  _quadTicket = _quadTexture = _quadWait = _quadCooldown = 0;',
     '''  _quadTicket = _quadTexture = _quadWait = _quadCooldown = 0;
  _surfaceSerial = _surfaceCount = _surfaceColor = 0;'''),
    ('  const char *source = geometryMode == kXenosQuadUploadAnisotropicBatch',
     '''  const char *source = geometryMode == kXenosSurfaceClear ? "Reusable Surface" :
                       geometryMode == kXenosQuadUploadAnisotropicBatch'''),
    ('    if (!submitQuad()) {', '''    const bool surface = _quadGeometryMode == kXenosSurfaceClear;
    if (!(surface ? submitSurfaceClear() : submitQuad())) {'''),
    ('''  if (_quadClient) {
    volatile UInt32 *pixels''',
     '''  if (_quadClient && _quadGeometryMode == kXenosSurfaceClear) {
    verifySurfaceClear(memory, fence);
  } else if (_quadClient) {
    volatile UInt32 *pixels'''),
    ('''  const char *result = state == kXenosQuadPassed ? "PASS" :''',
     '''  const char *result = state == kXenosQuadPassed ? "PASS" :'''),
    ('''  if (_quadGeometryMode == kXenosQuadUploadAnisotropicBatch) {
    setProperty("XenosBatchAnisotropicStripePixels",''',
     '''  if (_quadGeometryMode == kXenosSurfaceClear) {
    IOLog("XenosSurface: %s ticket=%lu fence=%lu pixels=%lu mismatches=%lu argb=%08lx hash=%08lx\\n",
          result, _quadTicket, fence, checked, mismatches, _surfaceColor, hash);
  } else if (_quadGeometryMode == kXenosQuadUploadAnisotropicBatch) {
    setProperty("XenosBatchAnisotropicStripePixels",'''),
    ('''  setGpuViewport(_fbWidth, _fbHeight);
  setGpuScissors(_fbWidth, _fbHeight);
  setResolveTarget(_gpuPhysAddr, _fbWidth, _fbHeight, kResolveConvert, kResolveDisplayFormat);

  writeRingPacket3(CP_MEM_WRITE, 2);''',
     '''  restoreQuadDisplayAndFence(fence);
  return true;
}

void XenosController::restoreQuadDisplayAndFence(UInt32 fence) {
  setGpuViewport(_fbWidth, _fbHeight);
  setGpuScissors(_fbWidth, _fbHeight);
  setResolveTarget(_gpuPhysAddr, _fbWidth, _fbHeight, kResolveConvert, kResolveDisplayFormat);

  writeRingPacket3(CP_MEM_WRITE, 2);'''),
    ('''  writeRing(_quadTicket);
  syncRingBuffer();
  return true;
}''', '''  writeRing(_quadTicket);
  syncRingBuffer();
}'''),
    ('void XenosController::clearQuadSampleTarget(UInt32 discard) {',
     'void XenosController::clearQuadSampleTarget(UInt32 discard, UInt32 clearColor) {'),
    ('''  writeRing(kSampleClearColor);
  writeRing(kSampleClearColor);''', '''  writeRing(clearColor);
  writeRing(clearColor);'''),
    ('  clearQuadSampleTarget(discard);', '  clearQuadSampleTarget(discard, kSampleClearColor);'),
])
text = controller.read_text().replace('clearQuadSampleTarget(base);', 'clearQuadSampleTarget(base, kSampleClearColor);')
implementation = (root / 'surface-controller.inc').read_text()
marker = 'bool XenosController::submitQuad() {'
assert text.count(marker) == 1
controller.write_text(text.replace(marker, implementation + '\n' + marker))
