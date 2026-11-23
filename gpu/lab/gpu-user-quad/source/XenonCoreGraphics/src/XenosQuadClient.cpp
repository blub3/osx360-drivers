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
    &XenosController::queueQuad, this, texture, &received);
  if (result == kIOReturnSuccess)
    *static_cast<UInt32 *>(ticket) = received;
  return result;
}

IOReturn XenosQuadClient::status(void *ticket, void *state, void *fence,
                                void *checked, void *mismatches, void *) {
  XenosController *controller = OSDynamicCast(XenosController, getProvider());
  if (!controller || isInactive())
    return kIOReturnNotReady;
  UInt32 result[kXenosQuadResultWords] = {0};
  const IOReturn callResult = controller->_quadGate->runAction(
    &XenosController::readQuadStatus, this, ticket, result);
  if (callResult == kIOReturnSuccess) {
    *static_cast<UInt32 *>(state) = result[0];
    *static_cast<UInt32 *>(fence) = result[1];
    *static_cast<UInt32 *>(checked) = result[2];
    *static_cast<UInt32 *>(mismatches) = result[3];
  }
  return callResult;
}
