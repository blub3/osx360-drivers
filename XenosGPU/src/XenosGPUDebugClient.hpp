//
//  XenosGPUDebugClient.hpp
//  Root-only debug and fence user client of XenosGPU (GPU plan step 1.2, docs/gpu-driver-plan.md).
//
//  Methods (IOConnectMethodScalarIScalarO; numbers in XenosGPUDebugProtocol.h):
//    read register, write an interrupt register, submit a fence, wait for a fence (polling),
//    statistics, GPU interrupt on/off. Memory type 0 maps the fence page read-only into the caller.
//

#ifndef XenosGPUDebugClient_hpp
#define XenosGPUDebugClient_hpp

#include <IOKit/IOUserClient.h>
#include "XenosGPUDebugProtocol.h"

class XenosGPU;

class XenosGPUDebugClient : public IOUserClient {
  OSDeclareDefaultStructors(XenosGPUDebugClient);
  typedef IOUserClient super;

public:
  bool initWithTask(task_t owningTask, void *securityToken, UInt32 type, OSDictionary *properties);
  bool start(IOService *provider);
  IOReturn clientClose(void);
  IOExternalMethod *getTargetAndMethodForIndex(IOService **target, UInt32 index);
  IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory);

  IOReturn readRegister(void *offset, void *value, void *, void *, void *, void *);
  IOReturn writeRegister(void *offset, void *value, void *, void *, void *, void *);
  IOReturn submitFence(void *sequence, void *, void *, void *, void *, void *);
  IOReturn waitFence(void *sequence, void *timeoutMS, void *elapsedUS, void *completed, void *, void *);
  IOReturn statistics(void *frames, void *busyTicks, void *fencesSubmitted, void *fenceCompleted,
                      void *interrupts, void *lastStatus);
  IOReturn interruptControl(void *enable, void *, void *, void *, void *, void *);

private:
  XenosGPU *_gpu;
  bool      _privileged;
};

#endif
