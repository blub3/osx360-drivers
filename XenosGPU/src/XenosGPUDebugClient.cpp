//
//  XenosGPUDebugClient.cpp
//  Root-only debug and fence user client of XenosGPU (GPU plan step 1.2).
//

#include "XenosGPUDebugClient.hpp"
#include "XenosGPU.hpp"

OSDefineMetaClassAndStructors(XenosGPUDebugClient, IOUserClient);

bool XenosGPUDebugClient::initWithTask(task_t owningTask, void *securityToken, UInt32 type,
                                       OSDictionary *properties) {
  _gpu = NULL;
  _privileged = (clientHasPrivilege(securityToken, kIOClientPrivilegeAdministrator) == kIOReturnSuccess);
  if (!_privileged) {
    IOLog("XenosGPU: debug client refused: not root\n");
    return false;
  }
  return super::initWithTask(owningTask, securityToken, type, properties);
}

bool XenosGPUDebugClient::start(IOService *provider) {
  _gpu = OSDynamicCast(XenosGPU, provider);
  if (_gpu == NULL || !super::start(provider)) {
    return false;
  }
  return true;
}

IOReturn XenosGPUDebugClient::clientClose(void) {
  terminate();
  return kIOReturnSuccess;
}

IOExternalMethod *XenosGPUDebugClient::getTargetAndMethodForIndex(IOService **target, UInt32 index) {
  static IOExternalMethod methods[kXenosGPUDebugMethodCount] = {
    { NULL, (IOMethod)&XenosGPUDebugClient::readRegister,     kIOUCScalarIScalarO, 1, 1 },
    { NULL, (IOMethod)&XenosGPUDebugClient::writeRegister,    kIOUCScalarIScalarO, 2, 0 },
    { NULL, (IOMethod)&XenosGPUDebugClient::submitFence,      kIOUCScalarIScalarO, 0, 1 },
    { NULL, (IOMethod)&XenosGPUDebugClient::waitFence,        kIOUCScalarIScalarO, 2, 2 },
    { NULL, (IOMethod)&XenosGPUDebugClient::statistics,       kIOUCScalarIScalarO, 0, 6 },
    { NULL, (IOMethod)&XenosGPUDebugClient::interruptControl, kIOUCScalarIScalarO, 1, 0 },
    { NULL, (IOMethod)&XenosGPUDebugClient::gpuWrite,         kIOUCScalarIScalarO, 2, 0 },
  };
  if (index >= kXenosGPUDebugMethodCount) {
    return NULL;
  }
  *target = this;
  return &methods[index];
}

IOReturn XenosGPUDebugClient::clientMemoryForType(UInt32 type, IOOptionBits *options,
                                                  IOMemoryDescriptor **memory) {
  *memory = NULL;
  if (type != kXenosGPUFenceMemoryType) {
    return kIOReturnUnsupported;
  }
  IOMemoryDescriptor *page = _gpu->fenceMemory();
  if (page == NULL) {
    return kIOReturnNotReady;
  }
  page->retain();
  *memory = page;
  *options |= kIOMapReadOnly | kIOMapInhibitCache;
  return kIOReturnSuccess;
}

IOReturn XenosGPUDebugClient::readRegister(void *offset, void *value, void *, void *, void *, void *) {
  return _gpu->debugRead((UInt32)(uintptr_t)offset, (UInt32 *)value);
}

IOReturn XenosGPUDebugClient::writeRegister(void *offset, void *value, void *, void *, void *, void *) {
  return _gpu->debugWrite((UInt32)(uintptr_t)offset, (UInt32)(uintptr_t)value);
}

IOReturn XenosGPUDebugClient::submitFence(void *sequence, void *, void *, void *, void *, void *) {
  return _gpu->submitFence((UInt32 *)sequence);
}

IOReturn XenosGPUDebugClient::waitFence(void *sequence, void *timeoutMS, void *elapsedUS, void *completed,
                                        void *, void *) {
  return _gpu->waitFence((UInt32)(uintptr_t)sequence, (UInt32)(uintptr_t)timeoutMS, (UInt32 *)elapsedUS,
                         (UInt32 *)completed);
}

IOReturn XenosGPUDebugClient::statistics(void *frames, void *busyTicks, void *fencesSubmitted,
                                         void *fenceCompleted, void *interrupts, void *lastStatus) {
  _gpu->debugStatistics((UInt32 *)frames, (UInt32 *)busyTicks, (UInt32 *)fencesSubmitted,
                        (UInt32 *)fenceCompleted, (UInt32 *)interrupts, (UInt32 *)lastStatus);
  return kIOReturnSuccess;
}

IOReturn XenosGPUDebugClient::gpuWrite(void *address, void *value, void *, void *, void *, void *) {
  return _gpu->debugGPUWrite((UInt32)(uintptr_t)address, (UInt32)(uintptr_t)value);
}

IOReturn XenosGPUDebugClient::interruptControl(void *enable, void *, void *, void *, void *, void *) {
  return _gpu->setInterrupt(enable != NULL);
}
