#include "XenosQuadClient.hpp"
#include "XenosController.hpp"

OSDefineMetaClassAndStructors(XenosQuadClient, IOUserClient);

bool XenosQuadClient::initWithTask(task_t task, void *securityToken, UInt32 type,
                                   OSDictionary *properties) {
  _surface = NULL;
  _surfaceHandle = _surfaceTicket = 0;
  _surfaceReady = false;
  return task && type == 0 && IOUserClient::initWithTask(task, securityToken, type, properties);
}

bool XenosQuadClient::start(IOService *provider) {
  XenosController *controller = OSDynamicCast(XenosController, provider);
  if (!controller || !controller->_quadGate || !IOUserClient::start(provider))
    return false;
  _ticket = 0;
  bzero(_result, sizeof(_result));
  return true;
}

void XenosQuadClient::stop(IOService *provider) {
  XenosController *controller = OSDynamicCast(XenosController, provider);
  if (controller && controller->_quadGate)
    controller->_quadGate->runAction(&XenosController::closeClientSurface, this);
  IOUserClient::stop(provider);
}

void XenosQuadClient::free() {
  if (_surface)
    _surface->release();
  IOUserClient::free();
}

IOReturn XenosQuadClient::clientClose() {
  terminate();
  return kIOReturnSuccess;
}

IOExternalMethod *XenosQuadClient::getTargetAndMethodForIndex(IOService **target, UInt32 selector) {
  static IOExternalMethod methods[] = {
    {NULL, (IOMethod)&XenosQuadClient::submit, kIOUCScalarIScalarO, 1, 1},
    {NULL, (IOMethod)&XenosQuadClient::status, kIOUCScalarIScalarO, 1, kXenosQuadResultWords},
    {NULL, (IOMethod)&XenosQuadClient::upload, kIOUCStructIStructO,
     kXenosQuadUploadBytes, sizeof(UInt32)},
    {NULL, (IOMethod)&XenosQuadClient::statusWithHash, kIOUCScalarIScalarO, 1,
     kXenosQuadUploadResultWords},
    {NULL, (IOMethod)&XenosQuadClient::uploadGeometry, kIOUCStructIStructO,
     sizeof(XenosQuadGeometryUpload), sizeof(UInt32)},
    {NULL, (IOMethod)&XenosQuadClient::uploadRotated, kIOUCStructIStructO,
     sizeof(XenosQuadGeometryUpload), sizeof(UInt32)},
    {NULL, (IOMethod)&XenosQuadClient::uploadBatch, kIOUCStructIStructO,
     sizeof(XenosQuadBatchUpload), sizeof(UInt32)},
    {NULL, (IOMethod)&XenosQuadClient::uploadBlendedBatch, kIOUCStructIStructO,
     sizeof(XenosQuadBlendedBatchUpload), sizeof(UInt32)},
    {NULL, (IOMethod)&XenosQuadClient::uploadScissoredBatch, kIOUCStructIStructO,
     sizeof(XenosQuadScissoredBatchUpload), sizeof(UInt32)},
    {NULL, (IOMethod)&XenosQuadClient::uploadStencilBatch, kIOUCStructIStructO,
     sizeof(XenosQuadStencilBatchUpload), sizeof(UInt32)},
    {NULL, (IOMethod)&XenosQuadClient::uploadScissorStencilBatch, kIOUCStructIStructO,
     sizeof(XenosQuadScissorStencilBatchUpload), sizeof(UInt32)},
    {NULL, (IOMethod)&XenosQuadClient::uploadDepthBatch, kIOUCStructIStructO,
     sizeof(XenosQuadDepthBatchUpload), sizeof(UInt32)},
    {NULL, (IOMethod)&XenosQuadClient::uploadRenderTextureBatch, kIOUCStructIStructO,
     sizeof(XenosQuadDepthBatchUpload), sizeof(UInt32)},
    {NULL, (IOMethod)&XenosQuadClient::uploadBilinearBatch, kIOUCStructIStructO,
     sizeof(XenosQuadDepthBatchUpload), sizeof(UInt32)},
    {NULL, (IOMethod)&XenosQuadClient::uploadMipmapBatch, kIOUCStructIStructO,
     sizeof(XenosQuadDepthBatchUpload), sizeof(UInt32)},
    {NULL, (IOMethod)&XenosQuadClient::uploadTrilinearBatch, kIOUCStructIStructO,
     sizeof(XenosQuadDepthBatchUpload), sizeof(UInt32)},
    {NULL, (IOMethod)&XenosQuadClient::uploadAnisotropicBatch, kIOUCStructIStructO,
     sizeof(XenosQuadDepthBatchUpload), sizeof(UInt32)},
    {NULL, (IOMethod)&XenosQuadClient::createSurface, kIOUCScalarIScalarO, 2, 1},
    {NULL, (IOMethod)&XenosQuadClient::clearSurface, kIOUCScalarIScalarO, 2, 1},
    {NULL, (IOMethod)&XenosQuadClient::readSurface, kIOUCStructIStructO,
     sizeof(XenosSurfaceReadRequest), sizeof(XenosSurfaceReadback)},
    {NULL, (IOMethod)&XenosQuadClient::releaseSurface, kIOUCScalarIScalarO, 1, 0},
  };

  if (selector >= sizeof(methods) / sizeof(methods[0]))
    return NULL;
  *target = this;
  return &methods[selector];
}

IOReturn XenosQuadClient::submit(void *texture, void *ticket, void *, void *, void *, void *) {
  XenosController *controller = OSDynamicCast(XenosController, getProvider());
  if (!controller || isInactive())
    return kIOReturnNotReady;
  UInt32 received = 0;
  const IOReturn result = controller->_quadGate->runAction(
    &XenosController::queueQuad, this, NULL, &received, texture);
  if (result == kIOReturnSuccess)
    *static_cast<UInt32 *>(ticket) = received;
  return result;
}

IOReturn XenosQuadClient::upload(void *rgb, void *ticket, void *inputSize,
                                 void *outputSize, void *, void *) {
  return sendUpload(rgb, ticket, inputSize, outputSize, kXenosQuadUploadBytes, &XenosController::queueQuad);
}

IOReturn XenosQuadClient::uploadGeometry(void *input, void *ticket, void *inputSize,
                                         void *outputSize, void *, void *) {
  return sendUpload(input, ticket, inputSize, outputSize, sizeof(XenosQuadGeometryUpload),
                    &XenosController::queueQuadGeometry);
}

IOReturn XenosQuadClient::uploadRotated(void *input, void *ticket, void *inputSize,
                                        void *outputSize, void *, void *) {
  return sendUpload(input, ticket, inputSize, outputSize, sizeof(XenosQuadGeometryUpload),
                    &XenosController::queueQuadRotated);
}

IOReturn XenosQuadClient::uploadBatch(void *input, void *ticket, void *inputSize,
                                      void *outputSize, void *, void *) {
  return sendUpload(input, ticket, inputSize, outputSize, sizeof(XenosQuadBatchUpload),
                    &XenosController::queueQuadBatch);
}

IOReturn XenosQuadClient::uploadBlendedBatch(void *input, void *ticket, void *inputSize,
                                             void *outputSize, void *, void *) {
  return sendUpload(input, ticket, inputSize, outputSize, sizeof(XenosQuadBlendedBatchUpload),
                    &XenosController::queueQuadBlendedBatch);
}

IOReturn XenosQuadClient::uploadScissoredBatch(void *input, void *ticket, void *inputSize,
                                               void *outputSize, void *, void *) {
  return sendUpload(input, ticket, inputSize, outputSize, sizeof(XenosQuadScissoredBatchUpload),
                    &XenosController::queueQuadScissoredBatch);
}

IOReturn XenosQuadClient::uploadStencilBatch(void *input, void *ticket, void *inputSize,
                                             void *outputSize, void *, void *) {
  return sendUpload(input, ticket, inputSize, outputSize, sizeof(XenosQuadStencilBatchUpload),
                    &XenosController::queueQuadStencilBatch);
}

IOReturn XenosQuadClient::uploadScissorStencilBatch(void *input, void *ticket, void *inputSize,
                                                    void *outputSize, void *, void *) {
  return sendUpload(input, ticket, inputSize, outputSize, sizeof(XenosQuadScissorStencilBatchUpload),
                    &XenosController::queueQuadScissorStencilBatch);
}

IOReturn XenosQuadClient::uploadDepthBatch(void *input, void *ticket, void *inputSize,
                                           void *outputSize, void *, void *) {
  return sendUpload(input, ticket, inputSize, outputSize, sizeof(XenosQuadDepthBatchUpload),
                    &XenosController::queueQuadDepthBatch);
}

IOReturn XenosQuadClient::uploadRenderTextureBatch(void *input, void *ticket, void *inputSize,
                                                    void *outputSize, void *, void *) {
  return sendUpload(input, ticket, inputSize, outputSize, sizeof(XenosQuadDepthBatchUpload),
                    &XenosController::queueQuadRenderTextureBatch);
}

IOReturn XenosQuadClient::uploadBilinearBatch(void *input, void *ticket, void *inputSize,
                                              void *outputSize, void *, void *) {
  return sendUpload(input, ticket, inputSize, outputSize, sizeof(XenosQuadDepthBatchUpload),
                    &XenosController::queueQuadBilinearBatch);
}

IOReturn XenosQuadClient::uploadMipmapBatch(void *input, void *ticket, void *inputSize,
                                            void *outputSize, void *, void *) {
  return sendUpload(input, ticket, inputSize, outputSize, sizeof(XenosQuadDepthBatchUpload),
                    &XenosController::queueQuadMipmapBatch);
}

IOReturn XenosQuadClient::uploadTrilinearBatch(void *input, void *ticket, void *inputSize,
                                            void *outputSize, void *, void *) {
  return sendUpload(input, ticket, inputSize, outputSize, sizeof(XenosQuadDepthBatchUpload),
                    &XenosController::queueQuadTrilinearBatch);
}

IOReturn XenosQuadClient::uploadAnisotropicBatch(void *input, void *ticket, void *inputSize,
                                            void *outputSize, void *, void *) {
  return sendUpload(input, ticket, inputSize, outputSize, sizeof(XenosQuadDepthBatchUpload),
                    &XenosController::queueQuadAnisotropicBatch);
}

IOReturn XenosQuadClient::surfaceAction(IOCommandGate::Action action, void *arg0,
                                        void *arg1, void *arg2) {
  XenosController *controller = OSDynamicCast(XenosController, getProvider());
  if (!controller || isInactive())
    return kIOReturnNotReady;
  return controller->_quadGate->runAction(action, this, arg0, arg1, arg2);
}

IOReturn XenosQuadClient::createSurface(void *width, void *height, void *handle,
                                        void *, void *, void *) {
  return surfaceAction(&XenosController::createClientSurface, width, height, handle);
}

IOReturn XenosQuadClient::clearSurface(void *handle, void *argb, void *ticket,
                                       void *, void *, void *) {
  return surfaceAction(&XenosController::queueSurfaceClear, handle, argb, ticket);
}

IOReturn XenosQuadClient::readSurface(void *input, void *output, void *inputSize,
                                      void *outputSize, void *, void *) {
  if (!input || !output || !outputSize ||
      reinterpret_cast<uintptr_t>(inputSize) != sizeof(XenosSurfaceReadRequest) ||
      *static_cast<IOByteCount *>(outputSize) != sizeof(XenosSurfaceReadback))
    return kIOReturnBadArgument;
  return surfaceAction(&XenosController::readClientSurface, input, output, NULL);
}

IOReturn XenosQuadClient::releaseSurface(void *handle, void *, void *, void *, void *, void *) {
  return surfaceAction(&XenosController::releaseClientSurface, handle, NULL, NULL);
}

IOReturn XenosQuadClient::sendUpload(void *input, void *ticket, void *inputSize,
                                     void *outputSize, IOByteCount expectedSize, IOCommandGate::Action action) {
  IOByteCount *size = static_cast<IOByteCount *>(outputSize);
  if (!input || !ticket || !size ||
      reinterpret_cast<uintptr_t>(inputSize) != expectedSize || *size != sizeof(UInt32))
    return kIOReturnBadArgument;
  XenosController *controller = OSDynamicCast(XenosController, getProvider());
  if (!controller || isInactive())
    return kIOReturnNotReady;

  UInt32 received = 0;
  const IOReturn result = controller->_quadGate->runAction(action, this, input, &received);
  if (result == kIOReturnSuccess)
    memcpy(ticket, &received, sizeof(received));
  return result;
}

IOReturn XenosQuadClient::readStatus(void *ticket, UInt32 *result) {
  XenosController *controller = OSDynamicCast(XenosController, getProvider());
  if (!controller || isInactive())
    return kIOReturnNotReady;
  return controller->_quadGate->runAction(
    &XenosController::readQuadStatus, this, ticket, result);
}

IOReturn XenosQuadClient::status(void *ticket, void *state, void *fence,
                                void *checked, void *mismatches, void *) {
  return statusWithHash(ticket, state, fence, checked, mismatches, NULL);
}

IOReturn XenosQuadClient::statusWithHash(void *ticket, void *state, void *fence,
                                         void *checked, void *mismatches, void *hash) {
  UInt32 result[kXenosQuadUploadResultWords] = {0};
  const IOReturn callResult = readStatus(ticket, result);
  if (callResult == kIOReturnSuccess) {
    *static_cast<UInt32 *>(state) = result[0];
    *static_cast<UInt32 *>(fence) = result[1];
    *static_cast<UInt32 *>(checked) = result[2];
    *static_cast<UInt32 *>(mismatches) = result[3];
    if (hash)
      *static_cast<UInt32 *>(hash) = result[4];
  }
  return callResult;
}
