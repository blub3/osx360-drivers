#ifndef XenosQuadClient_hpp
#define XenosQuadClient_hpp

#include <IOKit/IOUserClient.h>
#include "XenosQuadProtocol.h"

class XenosController;

class XenosQuadClient : public IOUserClient {
  OSDeclareDefaultStructors(XenosQuadClient);
  friend class XenosController;

public:
  bool initWithTask(task_t task, void *securityToken, UInt32 type, OSDictionary *properties);
  bool start(IOService *provider);
  IOReturn clientClose();

protected:
  IOExternalMethod *getTargetAndMethodForIndex(IOService **target, UInt32 selector);

private:
  IOReturn submit(void *texture, void *ticket, void *, void *, void *, void *);
  IOReturn status(void *ticket, void *state, void *fence, void *checked, void *mismatches, void *);

  // The controller's command gate serializes updates and status reads.
  UInt32 _ticket;
  UInt32 _result[kXenosQuadResultWords];
};

#endif
