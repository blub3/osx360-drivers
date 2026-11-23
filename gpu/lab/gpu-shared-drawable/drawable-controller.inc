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
