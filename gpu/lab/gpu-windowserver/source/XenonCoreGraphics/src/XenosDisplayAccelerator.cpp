#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/graphics/IOAccelerator.h>

class XenosDisplayAccelerator : public IOAccelerator {
  OSDeclareDefaultStructors(XenosDisplayAccelerator);

public:
  bool start(IOService *provider);
  void stop(IOService *provider);
  IOReturn newUserClient(task_t, void *, UInt32, OSDictionary *, IOUserClient **);

private:
  static bool matchFramebuffer(void *target, void *reference, IOService *service);
  IONotifier *_matchNotifier;
  IOService *_framebuffer;
  char _registryPath[512];
};

OSDefineMetaClassAndStructors(XenosDisplayAccelerator, IOAccelerator)

bool XenosDisplayAccelerator::start(IOService *provider) {
  _framebuffer = NULL;
  _matchNotifier = NULL;
  if (!IOAccelerator::start(provider))
    return false;
  int length = sizeof(_registryPath);
  OSDictionary *match = serviceMatching("XenonFramebufferTest");
  if (!match || !getPath(_registryPath, &length, gIOServicePlane)) {
    if (match)
      match->release();
    return false;
  }

  // Advertise the native registry association. Zero capability bits keep the
  // stock compositor on its software path until the surface API is implemented.
  UInt32 caps = 0;
  setProperty("AccelCaps", &caps, sizeof(caps));
  setProperty("XenosWindowServerProbe", true);
  registerService();
  _matchNotifier = addNotification(gIOMatchedNotification, match,
                                    &XenosDisplayAccelerator::matchFramebuffer, this);
  if (!_matchNotifier)
    IOLog("XenosWindowServer: framebuffer association notification unavailable\n");
  return true;
}

bool XenosDisplayAccelerator::matchFramebuffer(void *target, void *, IOService *service) {
  XenosDisplayAccelerator *self = static_cast<XenosDisplayAccelerator *>(target);
  if (self->_framebuffer ||
      service->getParentEntry(gIOServicePlane) != self->getProvider() ||
      service->getProperty("IOAccelTypes"))
    return true;
  if (!service->setProperty("IOAccelIndex", UInt64(0), 32))
    return true;
  if (!service->setProperty("IOAccelTypes", self->_registryPath)) {
    service->removeProperty("IOAccelIndex");
    return true;
  }
  service->retain();
  self->_framebuffer = service;
  IOLog("XenosWindowServer: published native IOAccelerator association with zero caps\n");
  return true;
}

void XenosDisplayAccelerator::stop(IOService *provider) {
  if (_matchNotifier) {
    _matchNotifier->remove();
    _matchNotifier = NULL;
  }
  if (_framebuffer) {
    const OSString *path = OSDynamicCast(OSString, _framebuffer->getProperty("IOAccelTypes"));
    if (path && path->isEqualTo(_registryPath)) {
      _framebuffer->removeProperty("IOAccelTypes");
      _framebuffer->removeProperty("IOAccelIndex");
    }
    _framebuffer->release();
    _framebuffer = NULL;
  }
  IOAccelerator::stop(provider);
}

IOReturn XenosDisplayAccelerator::newUserClient(task_t, void *, UInt32, OSDictionary *,
                                               IOUserClient **client) {
  *client = NULL;
  return kIOReturnUnsupported;
}
