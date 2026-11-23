#include "XenosQuadProtocol.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
using UInt32 = uint32_t;
using IOOptionBits = unsigned;
using IOReturn = int;
enum {
    kIOReturnSuccess, kIOReturnBadArgument, kIOReturnBusy, kIOReturnNoResources,
    kIOReturnNotReady, kIOReturnUnsupported, kIOReturnNotPermitted, kIOMapReadOnly = 0x1000,
    kSurfaceAllocationLimit = 8
};
struct OSObject {
    bool stopped = false;
    bool isInactive() const { return stopped; }
};
struct IOMemoryDescriptor {
    unsigned references = 1;
    void retain() { ++references; }
    void release() { assert(references); --references; }
};
using IOBufferMemoryDescriptor = IOMemoryDescriptor;
struct XenosQuadClient : OSObject {
    IOBufferMemoryDescriptor *_surface, *_surfacePixels, *_drawableView;
    UInt32 _surfaceHandle, _surfaceTicket;
    bool _drawableReader, _surfaceReady;
};
struct XenosController : OSObject {
    IOBufferMemoryDescriptor *_drawableGrantPixels;
    XenosDrawableToken _drawableGrantToken;
    UInt32 _surfaceCount, _drawableReaders;
    bool ownsSurface(XenosQuadClient *, UInt32) const;
    void dropSurface(XenosQuadClient *);
    void revokeDrawableGrant();
    static IOReturn grantDrawable(OSObject *, void *, void *, void *, void *);
    static IOReturn openDrawable(OSObject *, void *, void *, void *, void *);
    static IOReturn getDrawableView(OSObject *, void *, void *, void *, void *);
    static IOReturn closeClientSurface(OSObject *, void *, void *, void *, void *);
};
static void read_random(void *output, unsigned bytes) {
    static UInt32 sample = 0x12345678;
    assert(bytes == sizeof(XenosDrawableToken));
    auto *token = static_cast<XenosDrawableToken *>(output);
    for (unsigned i = 0; i < kXenosDrawableTokenWords; ++i)
        token->words[i] = ++sample;
}
static void *scalar(UInt32 value) {
    return reinterpret_cast<void *>(static_cast<uintptr_t>(value));
}
bool XenosController::ownsSurface(XenosQuadClient *client, UInt32 handle) const {
  return handle && client->_surface && handle == client->_surfaceHandle;
}
void XenosController::dropSurface(XenosQuadClient *client) {
  if (client->_surface) {
    if (_drawableGrantPixels == client->_surfacePixels)
      revokeDrawableGrant();
    client->_surfacePixels->release();
    client->_surface->release();
    client->_surface = client->_surfacePixels = NULL;
    --_surfaceCount;
  }
  client->_surfaceHandle = client->_surfaceTicket = 0;
  client->_surfaceReady = false;
}
void XenosController::revokeDrawableGrant() {
  _drawableGrantPixels = NULL;
  bzero(&_drawableGrantToken, sizeof(_drawableGrantToken));
}
IOReturn XenosController::grantDrawable(OSObject *owner, void *clientArg, void *handleArg,
                                        void *tokenOut, void *) {
  XenosController *self = static_cast<XenosController *>(owner);
  XenosQuadClient *client = static_cast<XenosQuadClient *>(clientArg);
  if (self->isInactive() || client->isInactive())
    return kIOReturnNotReady;
  if (!self->ownsSurface(client, static_cast<UInt32>(reinterpret_cast<uintptr_t>(handleArg))))
    return kIOReturnBadArgument;
  if (!client->_surfaceReady)
    return kIOReturnNotReady;
  if (self->_drawableGrantPixels)
    return kIOReturnBusy;

  read_random(&self->_drawableGrantToken, sizeof(self->_drawableGrantToken));
  self->_drawableGrantPixels = client->_surfacePixels;
  *static_cast<XenosDrawableToken *>(tokenOut) = self->_drawableGrantToken;
  return kIOReturnSuccess;
}
IOReturn XenosController::openDrawable(OSObject *owner, void *clientArg, void *tokenArg,
                                       void *, void *) {
  XenosController *self = static_cast<XenosController *>(owner);
  XenosQuadClient *client = static_cast<XenosQuadClient *>(clientArg);
  if (self->isInactive() || client->isInactive())
    return kIOReturnNotReady;
  if (!client->_drawableReader)
    return kIOReturnUnsupported;
  if (client->_drawableView)
    return kIOReturnBusy;

  const XenosDrawableToken &token = *static_cast<const XenosDrawableToken *>(tokenArg);
  UInt32 difference = 0;
  for (UInt32 word = 0; word < kXenosDrawableTokenWords; ++word)
    difference |= token.words[word] ^ self->_drawableGrantToken.words[word];
  if (!self->_drawableGrantPixels || difference)
    return kIOReturnNotPermitted;
  if (self->_drawableReaders >= kSurfaceAllocationLimit)
    return kIOReturnNoResources;

  client->_drawableView = self->_drawableGrantPixels;
  client->_drawableView->retain();
  ++self->_drawableReaders;
  self->revokeDrawableGrant();
  return kIOReturnSuccess;
}
IOReturn XenosController::getDrawableView(OSObject *owner, void *clientArg, void *options,
                                          void *descriptor, void *) {
  XenosController *self = static_cast<XenosController *>(owner);
  XenosQuadClient *client = static_cast<XenosQuadClient *>(clientArg);
  if (self->isInactive() || client->isInactive() || !client->_drawableReader || !client->_drawableView)
    return kIOReturnNotReady;
  client->_drawableView->retain();
  *static_cast<IOOptionBits *>(options) = kIOMapReadOnly;
  *static_cast<IOMemoryDescriptor **>(descriptor) = client->_drawableView;
  return kIOReturnSuccess;
}
IOReturn XenosController::closeClientSurface(OSObject *owner, void *clientArg, void *, void *, void *) {
  XenosController *self = static_cast<XenosController *>(owner);
  XenosQuadClient *client = static_cast<XenosQuadClient *>(clientArg);
  self->dropSurface(client);
  if (client->_drawableView) {
    client->_drawableView->release();
    client->_drawableView = NULL;
    --self->_drawableReaders;
  }
  return kIOReturnSuccess;
}
int main() {
    XenosController device = {};
    XenosQuadClient owner = {}, first = {}, second = {};
    first._drawableReader = second._drawableReader = true;
    IOBufferMemoryDescriptor storage, pixels;
    owner._surface = &storage;
    owner._surfacePixels = &pixels;
    owner._surfaceHandle = 17;
    device._surfaceCount = 1;

    XenosDrawableToken token = {}, wrong = {};
    IOMemoryDescriptor *mapped = nullptr;
    IOOptionBits flags = 0;
    assert(device.grantDrawable(&device, &owner, scalar(17), &token, nullptr) == kIOReturnNotReady);
    owner._surfaceReady = true;
    assert(device.grantDrawable(&device, &owner, scalar(1), &token, nullptr) == kIOReturnBadArgument);
    assert(device.grantDrawable(&device, &owner, scalar(17), &token, nullptr) == kIOReturnSuccess);
    assert(device.grantDrawable(&device, &owner, scalar(17), &wrong, nullptr) == kIOReturnBusy);
    assert(device.getDrawableView(&device, &first, &flags, &mapped, nullptr) == kIOReturnNotReady);
    assert(device.openDrawable(&device, &first, &wrong, nullptr, nullptr) == kIOReturnNotPermitted);
    assert(device.openDrawable(&device, &owner, &token, nullptr, nullptr) == kIOReturnUnsupported);
    assert(device.openDrawable(&device, &first, &token, nullptr, nullptr) == kIOReturnSuccess);
    assert(device.openDrawable(&device, &second, &token, nullptr, nullptr) == kIOReturnNotPermitted);
    assert(device.getDrawableView(&device, &first, &flags, &mapped, nullptr) == kIOReturnSuccess);
    assert(flags == kIOMapReadOnly && mapped == &pixels && pixels.references == 3);
    assert(storage.references == 1);

    assert(device.grantDrawable(&device, &owner, scalar(17), &token, nullptr) == kIOReturnSuccess);
    assert(device.closeClientSurface(&device, &owner, nullptr, nullptr, nullptr) == kIOReturnSuccess);
    assert(storage.references == 0 && pixels.references == 2 && device._surfaceCount == 0);
    assert(device.openDrawable(&device, &second, &token, nullptr, nullptr) == kIOReturnNotPermitted);
    assert(device.closeClientSurface(&device, &first, nullptr, nullptr, nullptr) == kIOReturnSuccess);
    assert(pixels.references == 1 && device._drawableReaders == 0);
    mapped->release();
    assert(pixels.references == 0);
    puts("Drawable model passed owner checks, one-use grant, read-only mapping, revocation, and backing lifetime.");
}
