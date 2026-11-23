#ifndef XenosQuadClient_hpp
#define XenosQuadClient_hpp

#include <IOKit/IOCommandGate.h>
#include <IOKit/IOUserClient.h>
#include "XenosQuadProtocol.h"

class IOBufferMemoryDescriptor;
class XenosController;

class XenosQuadClient : public IOUserClient {
  OSDeclareDefaultStructors(XenosQuadClient);
  friend class XenosController;

public:
  bool initWithTask(task_t task, void *securityToken, UInt32 type, OSDictionary *properties);
  bool start(IOService *provider);
  void stop(IOService *provider);
  void free();
  IOReturn clientClose();

protected:
  IOExternalMethod *getTargetAndMethodForIndex(IOService **target, UInt32 selector);

private:
  IOReturn submit(void *texture, void *ticket, void *, void *, void *, void *);
  IOReturn status(void *ticket, void *state, void *fence, void *checked, void *mismatches, void *);
  IOReturn upload(void *rgb, void *ticket, void *inputSize, void *outputSize, void *, void *);
  IOReturn uploadGeometry(void *input, void *ticket, void *inputSize, void *outputSize, void *, void *);
  IOReturn uploadRotated(void *input, void *ticket, void *inputSize, void *outputSize, void *, void *);
  IOReturn uploadBatch(void *input, void *ticket, void *inputSize, void *outputSize, void *, void *);
  IOReturn uploadBlendedBatch(void *input, void *ticket, void *inputSize, void *outputSize, void *, void *);
  IOReturn uploadScissoredBatch(void *input, void *ticket, void *inputSize, void *outputSize, void *, void *);
  IOReturn uploadStencilBatch(void *input, void *ticket, void *inputSize, void *outputSize, void *, void *);
  IOReturn uploadScissorStencilBatch(void *input, void *ticket, void *inputSize, void *outputSize, void *, void *);
  IOReturn uploadDepthBatch(void *input, void *ticket, void *inputSize, void *outputSize, void *, void *);
  IOReturn uploadRenderTextureBatch(void *input, void *ticket, void *inputSize, void *outputSize, void *, void *);
  IOReturn uploadBilinearBatch(void *input, void *ticket, void *inputSize, void *outputSize, void *, void *);
  IOReturn uploadMipmapBatch(void *input, void *ticket, void *inputSize, void *outputSize, void *, void *);
  IOReturn uploadTrilinearBatch(void *input, void *ticket, void *inputSize, void *outputSize, void *, void *);
  IOReturn uploadAnisotropicBatch(void *input, void *ticket, void *inputSize, void *outputSize, void *, void *);
  IOReturn createSurface(void *width, void *height, void *handle, void *, void *, void *);
  IOReturn clearSurface(void *handle, void *argb, void *ticket, void *, void *, void *);
  IOReturn readSurface(void *input, void *output, void *inputSize, void *outputSize, void *, void *);
  IOReturn releaseSurface(void *handle, void *, void *, void *, void *, void *);
  IOReturn surfaceAction(IOCommandGate::Action action, void *arg0, void *arg1, void *arg2);
  IOReturn sendUpload(void *input, void *ticket, void *inputSize, void *outputSize,
                      IOByteCount expectedSize, IOCommandGate::Action action);
  IOReturn statusWithHash(void *ticket, void *state, void *fence,
                          void *checked, void *mismatches, void *hash);
  IOReturn readStatus(void *ticket, UInt32 *result);

  // The controller's command gate serializes updates and status reads.
  UInt32 _ticket;
  UInt32 _result[kXenosQuadUploadResultWords];
  IOBufferMemoryDescriptor *_surface;
  UInt32 _surfaceHandle;
  UInt32 _surfaceTicket;
  bool _surfaceReady;
};

#endif
