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

change(kext / 'Makefile', [('0.0.5d35', '0.0.5d36')])
change(kext / 'Info.plist', [
    ('\t\t<key>com.apple.kernel.iokit</key>',
     '\t\t<key>com.apple.kernel.bsd</key>\n\t\t<string>1.1</string>\n\t\t<key>com.apple.kernel.iokit</key>'),
])
change(src / 'XenosQuadProtocol.h', [
    ('  kXenosSurfaceRelease = 20,', '''  kXenosSurfaceRelease = 20,
  kXenosDrawableExport = 21,
  kXenosDrawableImport = 22,
  kXenosDrawableReaderType = 1,
  kXenosDrawableMemoryType = 1,
  kXenosDrawableTokenWords = 4,'''),
    ('// Read a band of 8 rows', '''typedef struct {
  uint32_t words[kXenosDrawableTokenWords];
} XenosDrawableToken;

// Read a band of 8 rows'''),
])
change(src / 'XenosQuadClient.hpp', [
    ('  IOReturn clientClose();', '''  IOReturn clientClose();
  IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory);'''),
    ('  IOReturn surfaceAction(', '''  IOReturn exportDrawable(void *handle, void *word0, void *word1, void *word2, void *word3, void *);
  IOReturn importDrawable(void *word0, void *word1, void *word2, void *word3, void *, void *);
  IOReturn surfaceAction('''),
    ('  IOBufferMemoryDescriptor *_surface;', '''  IOBufferMemoryDescriptor *_surface;
  IOBufferMemoryDescriptor *_surfacePixels;
  IOBufferMemoryDescriptor *_drawableView;
  bool _drawableReader;'''),
])
change(src / 'XenosQuadClient.cpp', [
    ('  _surface = NULL;', '  _surface = _surfacePixels = _drawableView = NULL;\n  _drawableReader = type == kXenosDrawableReaderType;'),
    ('  return task && type == 0 && IOUserClient::initWithTask(task, securityToken, type, properties);',
     '  return task && (type == 0 || _drawableReader) &&\n         IOUserClient::initWithTask(task, securityToken, type, properties);'),
    ('''  if (_surface)
    _surface->release();
  IOUserClient::free();''', '''  if (_surface)
    _surface->release();
  if (_surfacePixels)
    _surfacePixels->release();
  if (_drawableView)
    _drawableView->release();
  IOUserClient::free();'''),
    ('''    {NULL, (IOMethod)&XenosQuadClient::releaseSurface, kIOUCScalarIScalarO, 1, 0},''',
     '''    {NULL, (IOMethod)&XenosQuadClient::releaseSurface, kIOUCScalarIScalarO, 1, 0},
    {NULL, (IOMethod)&XenosQuadClient::exportDrawable, kIOUCScalarIScalarO, 1, kXenosDrawableTokenWords},
    {NULL, (IOMethod)&XenosQuadClient::importDrawable, kIOUCScalarIScalarO, kXenosDrawableTokenWords, 0},'''),
    ('  if (selector >= sizeof(methods) / sizeof(methods[0]))',
     '''  if (selector >= sizeof(methods) / sizeof(methods[0]) ||
      _drawableReader != (selector == kXenosDrawableImport))'''),
    ('IOReturn XenosQuadClient::sendUpload(', '''IOReturn XenosQuadClient::clientMemoryForType(UInt32 type, IOOptionBits *options,
                                               IOMemoryDescriptor **memory) {
  *memory = NULL;
  if (!_drawableReader || type != kXenosDrawableMemoryType)
    return kIOReturnUnsupported;
  return surfaceAction(&XenosController::getDrawableView, options, memory, NULL);
}

IOReturn XenosQuadClient::exportDrawable(void *handle, void *word0, void *word1, void *word2,
                                         void *word3, void *) {
  XenosDrawableToken token;
  const IOReturn result = surfaceAction(&XenosController::grantDrawable, handle, &token, NULL);
  if (result == kIOReturnSuccess) {
    *static_cast<UInt32 *>(word0) = token.words[0];
    *static_cast<UInt32 *>(word1) = token.words[1];
    *static_cast<UInt32 *>(word2) = token.words[2];
    *static_cast<UInt32 *>(word3) = token.words[3];
  }
  return result;
}

IOReturn XenosQuadClient::importDrawable(void *word0, void *word1, void *word2, void *word3,
                                         void *, void *) {
  XenosDrawableToken token = {{
    static_cast<UInt32>(reinterpret_cast<uintptr_t>(word0)),
    static_cast<UInt32>(reinterpret_cast<uintptr_t>(word1)),
    static_cast<UInt32>(reinterpret_cast<uintptr_t>(word2)),
    static_cast<UInt32>(reinterpret_cast<uintptr_t>(word3))
  }};
  return surfaceAction(&XenosController::openDrawable, &token, NULL, NULL);
}

IOReturn XenosQuadClient::sendUpload('''),
])
change(src / 'XenosController.hpp', [
    ('  UInt32                     _surfaceColor;', '''  UInt32                     _surfaceColor;
  IOBufferMemoryDescriptor   *_surfaceJobPixels;
  IOBufferMemoryDescriptor   *_drawableGrantPixels;
  XenosDrawableToken          _drawableGrantToken;
  UInt32                     _drawableReaders;'''),
    ('  void verifySurfaceClear(UInt8 *memory, UInt32 fence);', '''  void verifySurfaceClear(UInt32 fence);
  static IOReturn grantDrawable(OSObject *owner, void *client, void *handle, void *token, void *);
  static IOReturn openDrawable(OSObject *owner, void *client, void *token, void *, void *);
  static IOReturn getDrawableView(OSObject *owner, void *client, void *options, void *descriptor, void *);
  void revokeDrawableGrant();'''),
])
controller = src / 'XenosController.cpp'
change(controller, [
    ('#include <IOKit/IOPlatformExpert.h>', '#include <IOKit/IOPlatformExpert.h>\n#include <sys/random.h>'),
    ('static const UInt32 kSurfaceStorageBytes = kQuadFenceOffset + PAGE_SIZE;',
     '''static const UInt32 kSurfaceFenceOffset = PAGE_SIZE;
static const UInt32 kSurfaceStorageBytes = kSurfaceFenceOffset + PAGE_SIZE;'''),
    ('  _surfaceSerial = _surfaceCount = _surfaceColor = 0;', '''  _surfaceSerial = _surfaceCount = _surfaceColor = _drawableReaders = 0;
  _surfaceJobPixels = _drawableGrantPixels = NULL;
  bzero(&_drawableGrantToken, sizeof(_drawableGrantToken));'''),
    ('''  volatile UInt32 *slot = reinterpret_cast<volatile UInt32 *>(memory + kQuadFenceOffset);
  flushMarkerCacheLine(slot);
  const UInt32 fence = *slot;''',
     '''  const bool surface = _quadGeometryMode == kXenosSurfaceClear;
  const UInt32 fenceOffset = surface ? kSurfaceFenceOffset : kQuadFenceOffset;
  volatile UInt32 *slot = reinterpret_cast<volatile UInt32 *>(memory + fenceOffset);
  flushMarkerCacheLine(slot);
  const UInt32 fence = *slot;'''),
    ('''  if (_quadClient && _quadGeometryMode == kXenosSurfaceClear) {
    verifySurfaceClear(memory, fence);''', '''  if (_quadClient && surface) {
    verifySurfaceClear(fence);'''),
    ('''  _quadScratch->release();
  _quadScratch = NULL;
  return true;''',
     '''  _quadScratch->release();
  _quadScratch = NULL;
  if (_surfaceJobPixels) {
    _surfaceJobPixels->release();
    _surfaceJobPixels = NULL;
  }
  return true;'''),
    ('''IOReturn XenosController::createClientSurface''', '''static IOBufferMemoryDescriptor *allocateSurfaceBuffer(IOByteCount bytes) {
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

IOReturn XenosController::createClientSurface'''),
    ('''  IOBufferMemoryDescriptor *surface = IOBufferMemoryDescriptor::withOptions(
      kIOMemoryPhysicallyContiguous, kSurfaceStorageBytes, PAGE_SIZE);
  if (!surface)
    return kIOReturnNoMemory;
  IOByteCount length = 0;
  const IOPhysicalAddress physical = surface->getPhysicalSegment(0, &length);
  if (!surface->getBytesNoCopy() || !physical || length < kSurfaceStorageBytes ||
      (physical & (PAGE_SIZE - 1)) || physical > kDisplayMemoryStart - kSurfaceStorageBytes) {
    surface->release();
    return kIOReturnNoResources;
  }''',
     '''  IOBufferMemoryDescriptor *surface = allocateSurfaceBuffer(kSurfaceStorageBytes);
  if (!surface)
    return kIOReturnNoResources;
  // A distinct allocation makes the mapping exactly one GPU pixel page.
  // The scratch space and the GPU fence remain kernel-only.
  IOBufferMemoryDescriptor *pixels = allocateSurfaceBuffer(PAGE_SIZE);
  if (!pixels) {
    surface->release();
    return kIOReturnNoResources;
  }'''),
    ('  client->_surface = surface;', '  client->_surface = surface;\n  client->_surfacePixels = pixels;'),
    ('''  const UInt8 *memory = static_cast<const UInt8 *>(client->_surface->getBytesNoCopy());
  const volatile UInt32 *tiled = reinterpret_cast<const volatile UInt32 *>(memory + kQuadReadbackOffset);''',
     '''  const volatile UInt32 *tiled =
      static_cast<const volatile UInt32 *>(client->_surfacePixels->getBytesNoCopy());'''),
    ('''  if (client->_surface) {
    client->_surface->release();
    client->_surface = NULL;
    --_surfaceCount;
  }''',
     '''  if (client->_surface) {
    if (_drawableGrantPixels == client->_surfacePixels)
      revokeDrawableGrant();
    client->_surfacePixels->release();
    client->_surface->release();
    client->_surface = client->_surfacePixels = NULL;
    --_surfaceCount;
  }'''),
    ('''  static_cast<XenosController *>(owner)->dropSurface(static_cast<XenosQuadClient *>(clientArg));
  return kIOReturnSuccess;
}

bool XenosController::submitSurfaceClear()''',
     '''  XenosController *self = static_cast<XenosController *>(owner);
  XenosQuadClient *client = static_cast<XenosQuadClient *>(clientArg);
  self->dropSurface(client);
  if (client->_drawableView) {
    client->_drawableView->release();
    client->_drawableView = NULL;
    --self->_drawableReaders;
  }
  return kIOReturnSuccess;
}

bool XenosController::submitSurfaceClear()'''),
    ('''  const UInt32 readback = physical + kQuadReadbackOffset;
  UInt8 *memory = static_cast<UInt8 *>(_quadScratch->getBytesNoCopy());
  prepareQuadReadback(memory + kQuadReadbackOffset);
  volatile UInt32 *marker = reinterpret_cast<volatile UInt32 *>(memory + kQuadFenceOffset);''',
     '''  _surfaceJobPixels = _quadClient->_surfacePixels;
  _surfaceJobPixels->retain();
  const UInt32 readback = static_cast<UInt32>(_surfaceJobPixels->getPhysicalSegment(0, &length));
  UInt8 *memory = static_cast<UInt8 *>(_quadScratch->getBytesNoCopy());
  prepareQuadReadback(static_cast<UInt8 *>(_surfaceJobPixels->getBytesNoCopy()));
  volatile UInt32 *marker = reinterpret_cast<volatile UInt32 *>(memory + kSurfaceFenceOffset);'''),
    ('''  restoreQuadDisplayAndFence(physical + kQuadFenceOffset);
  return true;
}

void XenosController::verifySurfaceClear(UInt8 *memory, UInt32 fence) {
  volatile UInt32 *pixels = reinterpret_cast<volatile UInt32 *>(memory + kQuadReadbackOffset);''',
     '''  restoreQuadDisplayAndFence(physical + kSurfaceFenceOffset);
  return true;
}

void XenosController::verifySurfaceClear(UInt32 fence) {
  volatile UInt32 *pixels = static_cast<volatile UInt32 *>(_surfaceJobPixels->getBytesNoCopy());'''),
    ('bool XenosController::submitQuad() {',
     (root / 'drawable-controller.inc').read_text() + '\nbool XenosController::submitQuad() {'),
])
