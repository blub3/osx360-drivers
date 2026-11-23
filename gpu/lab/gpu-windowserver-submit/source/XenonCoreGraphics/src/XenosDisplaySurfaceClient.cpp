#include "XenosDisplaySurfaceClient.hpp"
#include "XenosController.hpp"
#include "XenosDisplaySurfaceProtocol.h"

#include <IOKit/graphics/IOAccelSurfaceConnect.h>
#include <kern/task.h>

extern "C" void proc_selfname(char *, int);

OSDefineMetaClassAndStructors(XenosDisplaySurfaceClient, XenosQuadClient);

static const IOByteCount kReadbackBytes = kXenosSurfaceSide * kXenosSurfaceSide * sizeof(UInt32);
typedef char TigerSurfaceInformationSize[sizeof(XenosTigerSurfaceInfo) == 68 ? 1 : -1];
typedef char TigerSurfaceRegionSize[sizeof(IOAccelDeviceRegion) == 12 ? 1 : -1];

bool XenosDisplaySurfaceClient::initWithTask(task_t task, void *securityToken, UInt32 type,
                                            OSDictionary *properties) {
  _task = NULL;
  _identified = _nextColor = false;
  _readMapping = NULL;
  if (!XenosQuadClient::initWithTask(task, securityToken, type, properties))
    return false;
  char process[32] = {0};
  proc_selfname(process, sizeof(process));
  _windowServerSurface = !strcmp(process, "WindowServer");
  task_reference(task);
  _task = task;
  return true;
}

void XenosDisplaySurfaceClient::stop(IOService *provider) {
  XenosController *controller = OSDynamicCast(XenosController, provider);
  if (controller && controller->_quadGate)
    controller->_quadGate->runAction(&XenosDisplaySurfaceClient::unlockReadback, this);
  XenosQuadClient::stop(provider);
}

void XenosDisplaySurfaceClient::free() {
  if (_readMapping)
    _readMapping->release();
  if (_task)
    task_deallocate(_task);
  XenosQuadClient::free();
}

IOExternalMethod *XenosDisplaySurfaceClient::getTargetAndMethodForIndex(IOService **target,
                                                                       UInt32 selector) {
  static IOExternalMethod id = {NULL, (IOMethod)&XenosDisplaySurfaceClient::setID,
                               kIOUCScalarIScalarO, 2, 0};
  static IOExternalMethod shape = {NULL, (IOMethod)&XenosDisplaySurfaceClient::setShape,
                                  kIOUCScalarIStructI, 2, sizeof(IOAccelDeviceRegion)};
  static IOExternalMethod flushMethod = {NULL, (IOMethod)&XenosDisplaySurfaceClient::flush,
                                        kIOUCScalarIScalarO, 2, 0};
  static IOExternalMethod lock = {NULL, (IOMethod)&XenosDisplaySurfaceClient::readLock,
                                 kIOUCScalarIStructO, 0, sizeof(XenosTigerSurfaceInfo)};
  static IOExternalMethod unlock = {NULL, (IOMethod)&XenosDisplaySurfaceClient::readUnlock,
                                   kIOUCScalarIScalarO, 0, 0};
  if (!_task || current_task() != _task || isInactive())
    return NULL;
  *target = this;
  switch (selector) {
    case kIOAccelSurfaceSetIDMode: return &id;
    case kIOAccelSurfaceSetShape: return &shape;
    case kIOAccelSurfaceFlush: return &flushMethod;
    case kIOAccelSurfaceReadLock: return &lock;
    case kIOAccelSurfaceReadUnlock: return &unlock;
    default: return NULL;
  }
}

IOReturn XenosDisplaySurfaceClient::setID(void *id, void *mode, void *, void *, void *, void *) {
  if (reinterpret_cast<uintptr_t>(id) != kXenosDiagnosticSurfaceID ||
      reinterpret_cast<uintptr_t>(mode) != (kIOAccelSurfaceModeColorDepth8888 |
                                            kIOAccelSurfaceModeWindowedBit))
    return kIOReturnUnsupported;
  return surfaceAction(&XenosDisplaySurfaceClient::registerID, NULL, NULL, NULL);
}

IOReturn XenosDisplaySurfaceClient::setShape(void *options, void *index, void *input,
                                             void *length, void *, void *) {
  if (options || index || !input || reinterpret_cast<uintptr_t>(length) != sizeof(IOAccelDeviceRegion))
    return kIOReturnBadArgument;
  const IOAccelDeviceRegion *region = static_cast<const IOAccelDeviceRegion *>(input);
  if (region->num_rects || region->bounds.x != 0 || region->bounds.y != 0 ||
      region->bounds.w != kXenosSurfaceSide || region->bounds.h != kXenosSurfaceSide)
    return kIOReturnBadArgument;
  return surfaceAction(&XenosDisplaySurfaceClient::createBacking, NULL, NULL, NULL);
}

IOReturn XenosDisplaySurfaceClient::flush(void *mask, void *options, void *, void *, void *, void *) {
  if (reinterpret_cast<uintptr_t>(mask) != 1 || options)
    return kIOReturnBadArgument;
  return surfaceAction(&XenosDisplaySurfaceClient::submitClear, NULL, NULL, NULL);
}

IOReturn XenosDisplaySurfaceClient::readLock(void *output, void *length, void *, void *, void *, void *) {
  if (!output || !length || *static_cast<IOByteCount *>(length) != sizeof(XenosTigerSurfaceInfo))
    return kIOReturnBadArgument;
  return surfaceAction(&XenosDisplaySurfaceClient::lockReadback, output, NULL, NULL);
}

IOReturn XenosDisplaySurfaceClient::readUnlock(void *, void *, void *, void *, void *, void *) {
  return surfaceAction(&XenosDisplaySurfaceClient::unlockReadback, NULL, NULL, NULL);
}

IOReturn XenosDisplaySurfaceClient::registerID(OSObject *, void *arg, void *, void *, void *) {
  XenosDisplaySurfaceClient *client = static_cast<XenosDisplaySurfaceClient *>(arg);
  if (client->isInactive() || client->_identified)
    return kIOReturnNotReady;
  client->_identified = true;
  return kIOReturnSuccess;
}

IOReturn XenosDisplaySurfaceClient::createBacking(OSObject *owner, void *arg, void *, void *, void *) {
  XenosDisplaySurfaceClient *client = static_cast<XenosDisplaySurfaceClient *>(arg);
  if (!client->_identified)
    return kIOReturnNotReady;
  UInt32 handle = 0;
  void *side = reinterpret_cast<void *>(kXenosSurfaceSide);
  return XenosController::createClientSurface(owner, client, side, side, &handle);
}

IOReturn XenosDisplaySurfaceClient::submitClear(OSObject *owner, void *arg, void *, void *, void *) {
  XenosDisplaySurfaceClient *client = static_cast<XenosDisplaySurfaceClient *>(arg);
  if (client->isInactive() || !client->_surfaceHandle)
    return kIOReturnNotReady;
  if (client->_readMapping)
    return kIOReturnBusy;
  const UInt32 color = client->_nextColor ? kXenosDiagnosticSurfaceSecond : kXenosDiagnosticSurfaceFirst;
  UInt32 ticket = 0;
  const IOReturn result = XenosController::queueSurfaceClear(owner, client,
      reinterpret_cast<void *>(client->_surfaceHandle), reinterpret_cast<void *>(color), &ticket);
  if (result == kIOReturnSuccess)
    client->_nextColor = !client->_nextColor;
  return result;
}

IOReturn XenosDisplaySurfaceClient::lockReadback(OSObject *owner, void *arg, void *output, void *, void *) {
  XenosDisplaySurfaceClient *client = static_cast<XenosDisplaySurfaceClient *>(arg);
  if (client->isInactive() || !client->_surfaceReady)
    return kIOReturnNotReady;
  if (client->_readMapping)
    return kIOReturnBusy;

  // Map only a dedicated, fully initialized pixel page into the owning task.
  IOBufferMemoryDescriptor *linear = IOBufferMemoryDescriptor::withOptions(
      kIOMemoryKernelUserShared, kReadbackBytes, PAGE_SIZE);
  if (!linear)
    return kIOReturnNoMemory;
  void *bytes = linear->getBytesNoCopy();
  if (!bytes) {
    linear->release();
    return kIOReturnNoMemory;
  }

  for (UInt32 row = 0; row < kXenosSurfaceSide; row += kXenosSurfaceReadRows) {
    XenosSurfaceReadRequest request = {client->_surfaceHandle, client->_surfaceTicket, row};
    XenosSurfaceReadback band;
    const IOReturn result = XenosController::readClientSurface(owner, client,
        &request, &band, NULL);
    if (result != kIOReturnSuccess) {
      linear->release();
      return result;
    }
    memcpy(static_cast<UInt32 *>(bytes) + row * kXenosSurfaceSide, band.argb, sizeof(band.argb));
  }
  IOMemoryMap *mapping = linear->map(client->_task, 0, kIOMapAnywhere | kIOMapReadOnly, 0, kReadbackBytes);
  linear->release();
  if (!mapping)
    return kIOReturnNoMemory;
  client->_readMapping = mapping;
  XenosTigerSurfaceInfo *info = static_cast<XenosTigerSurfaceInfo *>(output);
  bzero(info, sizeof(*info));
  info->address[0] = mapping->getVirtualAddress();
  info->rowBytes = kXenosSurfaceSide * sizeof(UInt32);
  info->width = info->height = kXenosSurfaceSide;
  info->pixelFormat = kXenosDiagnosticSurfaceFormat;
  return kIOReturnSuccess;
}

IOReturn XenosDisplaySurfaceClient::unlockReadback(OSObject *, void *arg, void *, void *, void *) {
  XenosDisplaySurfaceClient *client = static_cast<XenosDisplaySurfaceClient *>(arg);
  if (!client->_readMapping)
    return kIOReturnNotReady;
  client->_readMapping->release();
  client->_readMapping = NULL;
  return kIOReturnSuccess;
}
