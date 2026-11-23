from pathlib import Path

root = Path(__file__).resolve().parent
kext = root / 'source/XenonCoreGraphics'
src = kext / 'src'

def change(path, pairs):
    text = path.read_text()
    for old, new in pairs:
        assert old in text, (path, old)
        text = text.replace(old, new)
    path.write_text(text)

def add_variant(path, start, end):
    text = path.read_text()
    begin = text.index(start)
    finish = text.index(end, begin)
    original = text[begin:finish]
    path.write_text(text[:finish] + original.replace('Trilinear', 'Anisotropic') + text[finish:])

change(kext / 'Makefile', [('0.0.5d33', '0.0.5d34')])
change(src / 'XenosQuadProtocol.h', [('  kXenosQuadUploadTrilinearBatch = 15,',
    '  kXenosQuadUploadTrilinearBatch = 15,\n  kXenosQuadUploadAnisotropicBatch = 16,')])
client = src / 'XenosQuadClient.cpp'
add_variant(client, '    {NULL, (IOMethod)&XenosQuadClient::uploadTrilinearBatch', '  };')
add_variant(client, 'IOReturn XenosQuadClient::uploadTrilinearBatch', 'IOReturn XenosQuadClient::sendUpload')
add_variant(src / 'XenosQuadClient.hpp', '  IOReturn uploadTrilinearBatch(', '  IOReturn sendUpload(')
hdr = src / 'XenosController.hpp'
change(hdr, [
    ('  UInt32                     _quadTrilinearMaxDelta;', '''  UInt32                     _quadTrilinearMaxDelta;
  UInt32                     _quadAnisoControl;
  UInt32                     _quadAnisoDark;
  UInt32                     _quadAnisoLight;
  UInt32                     _quadAnisoDarkMax;
  UInt32                     _quadAnisoLightMin;'''),
    ('UInt32 lodBiasSteps = 0);', 'UInt32 lodBiasSteps = 0,\n                     UInt32 anisoFilter = 0);'),
    ('  static IOReturn queueQuadTrilinearBatch(OSObject *owner, void *client, void *input, void *output, void *);',
     '  static IOReturn queueQuadTrilinearBatch(OSObject *owner, void *client, void *input, void *output, void *);\n'
     '  static IOReturn queueQuadAnisotropicBatch(OSObject *owner, void *client, void *input, void *output, void *);'),
    ('  void syncGpuMemory(UInt32 address, UInt32 bytes, UInt32 actions);',
     '  void verifyQuadAnisotropicPass(volatile UInt32 *sample);\n'
     '  void syncGpuMemory(UInt32 address, UInt32 bytes, UInt32 actions);'),
])
change(src / 'XenosController_Setup.cpp', [
    ('enum sq_tex_filter mipFilter, UInt32 lodBiasSteps) {',
     'enum sq_tex_filter mipFilter, UInt32 lodBiasSteps, UInt32 anisoFilter) {'),
    ('    (UInt32(mipFilter) << 23));', '    (UInt32(mipFilter) << 23) |\n    (anisoFilter << 25));'),
])
controller = src / 'XenosController.cpp'
add_variant(controller, 'IOReturn XenosController::queueQuadTrilinearBatch', 'IOReturn XenosController::enqueueQuadDepthBatch')
change(controller, [
    ('static const UInt32 kQuadMipOffset = kQuadSampleOffset + PAGE_SIZE;',
     'static const UInt32 kQuadMipOffset = kQuadSampleOffset + PAGE_SIZE;\n'
     'static const UInt32 kQuadAnisoTextureOffset = kQuadMipOffset + PAGE_SIZE;'),
    ('static const UInt32 kMipTailX = 16;', '''static const UInt32 kMipTailX = 16;
static const UInt32 kAnisoRegionCount = 2;
static const UInt32 kAnisoLeft[] = {2, 18};
static const UInt32 kAnisoTop = 5;
static const UInt32 kAnisoWidth = 12;
static const UInt32 kAnisoHeight = 6;
static const UInt32 kAnisoSlopeDenominator = 4;
static const UInt32 kAnisoU = 8;
static const UInt32 kAnisoVSpan = 8 * kAnisoHeight;
static const UInt32 kAniso16To1 = 5; // Fetch constant encoding.
static const UInt32 kAnisoColorTag = 0xff0060a0;
static const UInt32 kAnisoDarkRed = 16;
static const UInt32 kAnisoLightRed = 240;
static const UInt32 kAnisoGrayRed = (kAnisoDarkRed + kAnisoLightRed) / 2;
static const UInt32 kAnisoColorTolerance = 2;
static const UInt32 kAnisoMinStripeDelta = 32;'''),
    ('  _quadTrilinearMatches = _quadTrilinearWitnesses = _quadTrilinearMaxDelta = 0;',
     '''  _quadTrilinearMatches = _quadTrilinearWitnesses = _quadTrilinearMaxDelta = 0;
  _quadAnisoControl = _quadAnisoDark = _quadAnisoLight = _quadAnisoDarkMax = 0;
  _quadAnisoLightMin = 255;'''),
    ('  const char *source = geometryMode == kXenosQuadUploadTrilinearBatch',
     '''  const char *source = geometryMode == kXenosQuadUploadAnisotropicBatch ? "Anisotropic Texture Batch" :
                       geometryMode == kXenosQuadUploadTrilinearBatch'''),
    ('         _quadGeometryMode == kXenosQuadUploadTrilinearBatch;',
     '''         _quadGeometryMode == kXenosQuadUploadTrilinearBatch ||
         _quadGeometryMode == kXenosQuadUploadAnisotropicBatch;'''),
    ('  const bool mipmap = trilinear || _quadGeometryMode == kXenosQuadUploadMipmapBatch;',
     '''  const bool mipmap = trilinear || _quadGeometryMode == kXenosQuadUploadMipmapBatch;
  const bool anisotropic = _quadGeometryMode == kXenosQuadUploadAnisotropicBatch;'''),
    ('    const UInt32 budget = mipmap ?', '    const UInt32 budget = mipmap || anisotropic ?'),
    ('    const char *blendLog = trilinear ?', '    const char *blendLog = anisotropic ? "XenosBatchAnisotropic" :\n                           trilinear ?'),
    ('''    else if (renderTexture)
      verifyQuadTexturePass''', '''    else if (anisotropic)
      verifyQuadAnisotropicPass(reinterpret_cast<volatile UInt32 *>(memory + kQuadSampleOffset));
    else if (renderTexture)
      verifyQuadTexturePass'''),
    ('void XenosController::finishQuad(', '''static UInt32 anisotropicStripeColor(UInt32 x) {
  return kAnisoColorTag | ((x & 1 ? kAnisoLightRed : kAnisoDarkRed) << 16);
}

static bool anisotropicQuadCoversPixel(UInt32 x, UInt32 y, UInt32 left) {
  if (x < left || x >= left + kAnisoWidth)
    return false;
  // Use doubled pixel-center coordinates to account for the 1:4 tilt.
  const int alongY = int(kAnisoSlopeDenominator) * (2 * (int(y) - int(kAnisoTop)) + 1) -
                     (2 * int(x - left) + 1);
  return alongY > 0 && alongY < int(2 * kAnisoSlopeDenominator * kAnisoHeight);
}

void XenosController::verifyQuadAnisotropicPass(volatile UInt32 *sample) {
  for (UInt32 offset = 0; offset < PAGE_SIZE; offset += CACHELINE_SIZE)
    flushMarkerCacheLine(sample + offset / sizeof(UInt32));

  _quadSampleHash = XENOS_QUAD_HASH_INITIAL;
  const UInt32 controlGray = kAnisoColorTag | (kAnisoGrayRed << 16);
  for (UInt32 y = 0; y < kClearSide; ++y) {
    for (UInt32 x = 0; x < kClearSide; ++x) {
      const UInt32 observed = sample[resolveTileWord(x, y)];
      const bool control = anisotropicQuadCoversPixel(x, y, kAnisoLeft[0]);
      const bool stripes = anisotropicQuadCoversPixel(x, y, kAnisoLeft[1]);
      bool matched = observed == kSampleBackground;
      if (control) {
        matched = quadMaxChannelDelta(observed, controlGray) <= kAnisoColorTolerance;
        _quadAnisoControl += matched;
      } else if (stripes) {
        const UInt32 red = (observed >> 16) & 0xff;
        const bool light = (kAnisoU + x - kAnisoLeft[1]) & 1;
        const bool tag = quadMaxChannelDelta(observed & 0xff00ffffU, kAnisoColorTag) <= kAnisoColorTolerance;
        matched = tag && (light ? red >= kAnisoGrayRed + kAnisoMinStripeDelta
                                : red <= kAnisoGrayRed - kAnisoMinStripeDelta);
        if (light) {
          _quadAnisoLight += matched;
          if (red < _quadAnisoLightMin)
            _quadAnisoLightMin = red;
        } else {
          _quadAnisoDark += matched;
          if (red > _quadAnisoDarkMax)
            _quadAnisoDarkMax = red;
        }
      } else {
        _quadSampleGuard += matched;
      }
      if (!matched && !_quadSampleMismatches)
        IOLog("XenosBatchAnisotropic: sample mismatch=(%lu,%lu) observed=%08lx control=%u stripes=%u\\n",
              x, y, observed, control, stripes);
      _quadSampleMismatches += !matched;
      _quadSampleHash = quadHashColor(_quadSampleHash, observed);
    }
  }
  _quadSampleChecked = kClearPixels;
}

void XenosController::finishQuad('''),
    ('''  if (_quadGeometryMode == kXenosQuadUploadTrilinearBatch) {
    setProperty("XenosBatchTrilinearWitnesses"''',
     '''  if (_quadGeometryMode == kXenosQuadUploadAnisotropicBatch) {
    setProperty("XenosBatchAnisotropicStripePixels", _quadAnisoDark + _quadAnisoLight, 32);
    setProperty("XenosBatchAnisotropicControlPixels", _quadAnisoControl, 32);
    IOLog("XenosBatchAnisotropic: %s ticket=%lu fence=%lu first_pixels=%lu first_mismatches=%lu "
          "sample_pixels=%lu sample_mismatches=%lu control_pixels=%lu dark_pixels=%lu light_pixels=%lu "
          "guard_pixels=%lu dark_max=%lu light_min=%lu first_hash=%08lx sample_hash=%08lx\\n",
          result, _quadTicket, fence, checked - _quadSampleChecked, mismatches - _quadSampleMismatches,
          _quadSampleChecked, _quadSampleMismatches, _quadAnisoControl, _quadAnisoDark, _quadAnisoLight,
          _quadSampleGuard, _quadAnisoDarkMax, _quadAnisoLightMin, hash, _quadSampleHash);
  } else if (_quadGeometryMode == kXenosQuadUploadTrilinearBatch) {
    setProperty("XenosBatchTrilinearWitnesses"'''),
    ('  const UInt32 scratchBytes = mipmap ? kQuadMipOffset + PAGE_SIZE :',
     '''  const UInt32 scratchBytes = anisotropic ? kQuadAnisoTextureOffset + PAGE_SIZE :
                              mipmap ? kQuadMipOffset + PAGE_SIZE :'''),
    ('''  if (mipmap) {
    // The 16x16 level''', '''  if (anisotropic) {
    for (UInt32 index = 0; index < kAnisoRegionCount; ++index) {
      const UInt32 left = kAnisoLeft[index];
      XenosQuadGeometry quad = xenosQuadMakeGeometry(left, kAnisoTop, left + kAnisoWidth,
          kAnisoTop + kAnisoHeight, kAnisoU, 0, kAnisoU + kAnisoWidth, kAnisoVSpan);
      quad.vertices[1].y += kAnisoWidth / kAnisoSlopeDenominator;
      quad.vertices[3].y += kAnisoWidth / kAnisoSlopeDenominator;
      packQuadSampleQuad(sampleGeometrySlot + index * kQuadGeometryStride, quad, 0, 0);
    }
    volatile UInt32 *striped = reinterpret_cast<volatile UInt32 *>(memory + kQuadAnisoTextureOffset);
    volatile UInt32 *mip = reinterpret_cast<volatile UInt32 *>(memory + kQuadMipOffset);
    for (UInt32 y = 0; y < kClearSide; ++y) {
      for (UInt32 x = 0; x < kClearSide; ++x) {
        const UInt32 texel = resolveTileWord(x, y);
        striped[texel] = anisotropicStripeColor(x);
        // Box-filtering each adjacent red pair yields a constant 16x16 mip.
        mip[texel] = x >= kMipTailX && y < kMipSide
            ? kAnisoColorTag | (kAnisoGrayRed << 16) : kClearSentinel;
      }
    }
    syncData(mip, 2 * PAGE_SIZE);
  } else if (mipmap) {
    // The 16x16 level'''),
    ('  const UInt32 sampleQuads = trilinear ?', '  const UInt32 sampleQuads = anisotropic ? kAnisoRegionCount :\n                             trilinear ?'),
    ('''  flushQuadResolve(readback);
  if (mipmap) {''', '''  flushQuadResolve(readback);
  if (anisotropic) {
    const UInt32 mip = base + kQuadMipOffset;
    const UInt32 striped = base + kQuadAnisoTextureOffset;
    syncGpuMemory(mip, 2 * PAGE_SIZE, kCoherVertexAndTextureCaches);
    clearQuadSampleTarget(base);
    beginQuadSampleDraw();
    for (UInt32 index = 0; index < kAnisoRegionCount; ++index) {
      const UInt32 position = vertices + sampleGeometryOffset + index * kQuadGeometryStride;
      // The two angled draws use identical texture gradients. Only the
      // maximum anisotropy changes, from disabled to 16:1.
      const UInt32 maxAniso = index == 0 ? 0 : kAniso16To1;
      drawTextureLayer(position, position + CACHELINE_SIZE, striped, SQ_TEX_FILTER_BILINEAR, mip,
                       SQ_TEX_FILTER_BILINEAR, 0, maxAniso);
    }
    resolveQuadColor(base + kQuadSampleOffset);
    flushQuadResolve(base + kQuadSampleOffset);
  } else if (mipmap) {'''),
    ('enum sq_tex_filter mipFilter, UInt32 lodBiasSteps) {',
     'enum sq_tex_filter mipFilter, UInt32 lodBiasSteps, UInt32 anisoFilter) {'),
    ('setGpuTexture(texture, kClearSide, kClearSide, filter, true, mipAddress, mipFilter, lodBiasSteps);',
     'setGpuTexture(texture, kClearSide, kClearSide, filter, true, mipAddress, mipFilter, lodBiasSteps, anisoFilter);'),
])
app = root / 'app/XenosQuadBatchTrilinearRetry.c'
target = app.with_name('XenosQuadBatchAnisotropic.c')
text = app.read_text().replace('XenosQuadBatchTrilinearRetry', 'XenosQuadBatchAnisotropic')
text = text.replace('kXenosQuadUploadTrilinearBatch', 'kXenosQuadUploadAnisotropicBatch')
text = text.replace('Boot the quad batch trilinear retry', 'Boot the quad batch anisotropic test')
text = text.replace('Trilinear batch upload failed', 'Anisotropic batch upload failed')
text = text.replace('mip_levels=2 lods=0,0.5,1 batch_draws=', 'mip_levels=2 max_anisotropy=16 batch_draws=')
text = text.replace('    // The driver checks the batch image, the generated mip and all three regions\n'
                    '    // of the sampling pass before reporting one fence and aggregate result.',
                    '    // The GPU fence covers the clipped batch and both sampler draws.')
text = text.replace('kXenosQuadMipmapReadbackPixels', '2 * kXenosQuadReadbackPixels')
target.write_text(text)
app.unlink()
