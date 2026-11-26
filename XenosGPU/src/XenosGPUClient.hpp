//
//  XenosGPUClient.hpp
//  GPU client of XenosGPU for any process (GPU plan step 1.3). Interface in XenosGPUProtocol.h.
//

#ifndef XenosGPUClient_hpp
#define XenosGPUClient_hpp

#include <IOKit/IOUserClient.h>
#include "XenosGPUProtocol.h"

class XenosGPU;

class XenosGPUClient : public IOUserClient {
  OSDeclareDefaultStructors(XenosGPUClient);
  typedef IOUserClient super;

public:
  bool initWithTask(task_t owningTask, void *securityToken, UInt32 type, OSDictionary *properties);
  bool start(IOService *provider);
  IOReturn clientClose(void);
  IOExternalMethod *getTargetAndMethodForIndex(IOService **target, UInt32 index);
  IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory);

  IOReturn allocate(void *bytes, void *flags, void *handle, void *gpuAddress, void *allocated, void *);
  IOReturn release(void *handle, void *, void *, void *, void *, void *);
  IOReturn heapStatistics(void *heapBytes, void *freeBytes, void *allocations, void *largestFree, void *, void *);

private:
  XenosGPU *_gpu;
};

#endif
