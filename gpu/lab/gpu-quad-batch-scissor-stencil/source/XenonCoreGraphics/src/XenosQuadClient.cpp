#include "XenosQuadClient.hpp"
#include "XenosController.hpp"

OSDefineMetaClassAndStructors(XenosQuadClient, IOUserClient);

bool XenosQuadClient::initWithTask(task_t task, void *securityToken, UInt32 type,
                                   OSDictionary *properties) {
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
