#ifndef XENOS_DISPLAY_SURFACE_CLIENT_HPP
#define XENOS_DISPLAY_SURFACE_CLIENT_HPP

#include "XenosQuadClient.hpp"

class IOMemoryMap;

class XenosDisplaySurfaceClient : public XenosQuadClient {
  OSDeclareDefaultStructors(XenosDisplaySurfaceClient);

public:
  bool initWithTask(task_t task, void *securityToken, UInt32 type, OSDictionary *properties);
  void stop(IOService *provider);
  void free();

protected:
  IOExternalMethod *getTargetAndMethodForIndex(IOService **target, UInt32 selector);

private:
  IOReturn setID(void *id, void *mode, void *, void *, void *, void *);
  IOReturn setShape(void *options, void *index, void *region, void *length, void *, void *);
  IOReturn flush(void *mask, void *options, void *, void *, void *, void *);
  IOReturn readLock(void *output, void *length, void *, void *, void *, void *);
  IOReturn readUnlock(void *, void *, void *, void *, void *, void *);

  static IOReturn registerID(OSObject *, void *, void *, void *, void *);
  static IOReturn createBacking(OSObject *, void *, void *, void *, void *);
  static IOReturn submitClear(OSObject *, void *, void *, void *, void *);
  static IOReturn lockReadback(OSObject *, void *, void *, void *, void *);
  static IOReturn unlockReadback(OSObject *, void *, void *, void *, void *);

  task_t _task;
  bool _identified;
  bool _nextColor;
  IOMemoryMap *_readMapping;
};

#endif
