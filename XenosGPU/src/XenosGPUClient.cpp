//
//  XenosGPUClient.cpp
//  GPU client of XenosGPU for any process (GPU plan step 1.3): GPU memory. Command submission
//  with an address check follows in step 1.4.
//

#include "XenosGPUClient.hpp"
#include "XenosGPU.hpp"

OSDefineMetaClassAndStructors(XenosGPUClient, IOUserClient);

bool XenosGPUClient::initWithTask(task_t owningTask, void *securityToken, UInt32 type, OSDictionary *properties) {
  _gpu = NULL;
  return super::initWithTask(owningTask, securityToken, type, properties);
}

bool XenosGPUClient::start(IOService *provider) {
  _gpu = OSDynamicCast(XenosGPU, provider);
  return _gpu != NULL && super::start(provider);
}

IOReturn XenosGPUClient::clientClose(void) {
  UInt32 freed = _gpu != NULL ? _gpu->freeAllMemory(this) : 0;
  if (freed != 0) {
    IOLog("XenosGPU: client closed holding %lu allocations; freed\n", (unsigned long)freed);
  }
  terminate();
  return kIOReturnSuccess;
}

IOExternalMethod *XenosGPUClient::getTargetAndMethodForIndex(IOService **target, UInt32 index) {
  static IOExternalMethod methods[kXenosGPUClientMethodCount] = {
    { NULL, (IOMethod)&XenosGPUClient::allocate,       kIOUCScalarIScalarO, 2, 3 },
    { NULL, (IOMethod)&XenosGPUClient::release,        kIOUCScalarIScalarO, 1, 0 },
    { NULL, (IOMethod)&XenosGPUClient::heapStatistics, kIOUCScalarIScalarO, 0, 4 },
  };
  if (index >= kXenosGPUClientMethodCount) {
    return NULL;
  }
  *target = this;
  return &methods[index];
}

IOReturn XenosGPUClient::clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory) {
  *memory = _gpu->memoryForHandle(this, type);
  if (*memory == NULL) {
    return kIOReturnNotFound;
  }
  *options |= kIOMapInhibitCache;
  return kIOReturnSuccess;
}

IOReturn XenosGPUClient::allocate(void *bytes, void *flags, void *handle, void *gpuAddress, void *allocated, void *) {
  if (flags != NULL) {
    return kIOReturnBadArgument;
  }
  return _gpu->allocateMemory(this, (UInt32)(uintptr_t)bytes, (UInt32 *)handle, (UInt32 *)gpuAddress,
                              (UInt32 *)allocated);
}

IOReturn XenosGPUClient::release(void *handle, void *, void *, void *, void *, void *) {
  return _gpu->freeMemory(this, (UInt32)(uintptr_t)handle);
}

IOReturn XenosGPUClient::heapStatistics(void *heapBytes, void *freeBytes, void *allocations, void *largestFree,
                                        void *, void *) {
  _gpu->heapStatistics((UInt32 *)heapBytes, (UInt32 *)freeBytes, (UInt32 *)allocations, (UInt32 *)largestFree);
  return kIOReturnSuccess;
}
