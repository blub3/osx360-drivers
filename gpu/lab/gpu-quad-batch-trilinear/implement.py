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
    path.write_text(text[:finish] + original.replace('Mipmap', 'Trilinear') + text[finish:])

change(kext / 'Makefile', [('0.0.5d31', '0.0.5d32')])
change(src / 'XenosQuadProtocol.h', [('  kXenosQuadUploadMipmapBatch = 14,',
    '  kXenosQuadUploadMipmapBatch = 14,\n  kXenosQuadUploadTrilinearBatch = 15,')])
client = src / 'XenosQuadClient.cpp'
add_variant(client, '    {NULL, (IOMethod)&XenosQuadClient::uploadMipmapBatch', '  };')
add_variant(client, 'IOReturn XenosQuadClient::uploadMipmapBatch', 'IOReturn XenosQuadClient::sendUpload')
change(src / 'XenosQuadClient.hpp', [('  IOReturn uploadMipmapBatch(void *input, void *ticket, void *inputSize, void *outputSize, void *, void *);',
    '  IOReturn uploadMipmapBatch(void *input, void *ticket, void *inputSize, void *outputSize, void *, void *);\n'
    '  IOReturn uploadTrilinearBatch(void *input, void *ticket, void *inputSize, void *outputSize, void *, void *);')])
change(src / 'XenosController.hpp', [
    ('  UInt32                     _quadMipLevel1Evidence;', '''  UInt32                     _quadMipLevel1Evidence;
  UInt32                     _quadTrilinearMatches;
  UInt32                     _quadTrilinearWitnesses;
  UInt32                     _quadTrilinearMaxDelta;'''),
    ('enum sq_tex_filter filter, bool tiled, UInt32 mipAddress = 0);',
     '''enum sq_tex_filter filter, bool tiled, UInt32 mipAddress = 0,
                     enum sq_tex_filter mipFilter = SQ_TEX_FILTER_POINT, UInt32 lodBiasSteps = 0);'''),
    ('enum sq_tex_filter filter = SQ_TEX_FILTER_POINT, UInt32 mipAddress = 0);',
     '''enum sq_tex_filter filter = SQ_TEX_FILTER_POINT, UInt32 mipAddress = 0,
                        enum sq_tex_filter mipFilter = SQ_TEX_FILTER_POINT, UInt32 lodBiasSteps = 0);'''),
    ('  static IOReturn queueQuadMipmapBatch(OSObject *owner, void *client, void *input, void *output, void *);',
     '  static IOReturn queueQuadMipmapBatch(OSObject *owner, void *client, void *input, void *output, void *);\n'
     '  static IOReturn queueQuadTrilinearBatch(OSObject *owner, void *client, void *input, void *output, void *);'),
    ('  void verifyQuadMipmapPass(const volatile UInt32 *base, volatile UInt32 *mip, volatile UInt32 *sample);',
     '''  void verifyQuadMipmapPass(const volatile UInt32 *base, volatile UInt32 *mip, volatile UInt32 *sample);
  void verifyQuadTrilinearSamples(const volatile UInt32 *base, const volatile UInt32 *mip,
                                  const volatile UInt32 *sample);'''),
])
change(src / 'XenosController_Setup.cpp', [
    ('enum sq_tex_filter filter, bool tiled, UInt32 mipAddress) {',
     '''enum sq_tex_filter filter, bool tiled, UInt32 mipAddress,
                                      enum sq_tex_filter mipFilter, UInt32 lodBiasSteps) {'''),
    ('''    A2XX_SQ_TEX_3_XY_MIN_FILTER(filter));
  // Mip point filtering is zero in word 3. Word 4 selects levels 0..1
  // when a mip is present, and word 5 selects 2D and a packed mip tail.
  writeRing(mipAddress ? (1U << 6) : 0);''',
     '''    A2XX_SQ_TEX_3_XY_MIN_FILTER(filter) |
    (UInt32(mipFilter) << 23));
  // Word 4 selects levels 0..1 and encodes LOD bias in units of 1/32.
  // Word 5 selects 2D and the packed mip tail.
  writeRing(mipAddress ? ((1U << 6) | (lodBiasSteps << 12)) : 0);'''),
])
controller=src / 'XenosController.cpp'
add_variant(controller, 'IOReturn XenosController::queueQuadMipmapBatch', 'IOReturn XenosController::enqueueQuadDepthBatch')
change(controller, [
    ('static const UInt32 kMipTailX = 16;', '''static const UInt32 kMipTailX = 16;
static const UInt32 kTrilinearRegionCount = 3;
static const UInt32 kTrilinearRegionX[] = {1, 11, 21};
static const UInt32 kTrilinearRegionY = 1;
static const UInt32 kTrilinearWidth = 8;
static const UInt32 kTrilinearHeight = 16;
static const UInt32 kTrilinearU = 5;
static const UInt32 kTrilinearV = 6;
static const UInt32 kTrilinearUQuarter = 1;
static const UInt32 kTrilinearVQuarter = 3;
static const UInt32 kTrilinearLodSteps[] = {0, 16, 32};
// Account for fixed-point interpolation and framebuffer quantization.
static const UInt32 kTrilinearTolerance = 5;'''),
    ('''static UInt32 quadBilinearColor(const UInt32 *samples) {
  // The 1:1 tiled sample path was measured about 1/64 texel below the
  // programmed coordinates on both axes. The taps stay in the same 2x2 block.
  const UInt32 scale = 64;
  const UInt32 u = kSampleOffsetUQuarter * (scale / 4) - 1;
  const UInt32 v = kSampleOffsetVQuarter * (scale / 4) - 1;''',
     '''static UInt32 quadBilinearColor(const UInt32 *samples, UInt32 u, UInt32 v) {
  const UInt32 scale = 64;'''),
    ('expected = quadBilinearColor(neighbors);',
     '''// This path measures one 1/64-texel phase step below programmed UVs.
        expected = quadBilinearColor(neighbors, kSampleOffsetUQuarter * 16 - 1,
                                     kSampleOffsetVQuarter * 16 - 1);'''),
    ('  _quadMipBaseMatches = _quadMipLevel1Matches = _quadMipLevel1Evidence = 0;',
     '''  _quadMipBaseMatches = _quadMipLevel1Matches = _quadMipLevel1Evidence = 0;
  _quadTrilinearMatches = _quadTrilinearWitnesses = _quadTrilinearMaxDelta = 0;'''),
    ('  const char *source = geometryMode == kXenosQuadUploadMipmapBatch',
     '''  const char *source = geometryMode == kXenosQuadUploadTrilinearBatch ? "Trilinear Texture Batch" :
                       geometryMode == kXenosQuadUploadMipmapBatch'''),
    ('         _quadGeometryMode == kXenosQuadUploadMipmapBatch;',
     '''         _quadGeometryMode == kXenosQuadUploadMipmapBatch ||
         _quadGeometryMode == kXenosQuadUploadTrilinearBatch;'''),
    ('  const bool mipmap = _quadGeometryMode == kXenosQuadUploadMipmapBatch;',
     '''  const bool trilinear = _quadGeometryMode == kXenosQuadUploadTrilinearBatch;
  const bool mipmap = trilinear || _quadGeometryMode == kXenosQuadUploadMipmapBatch;'''),
    ('    const char *blendLog = mipmap ? "XenosBatchMipmap" :',
     '''    const char *blendLog = trilinear ? "XenosBatchTrilinear" :
                           mipmap ? "XenosBatchMipmap" :'''),
    ('    finishQuad(failures ? kXenosQuadPixelsFailed : kXenosQuadPassed,',
     '''    const bool missingBlend = trilinear && !_quadTrilinearWitnesses;
    finishQuad(failures || missingBlend ? kXenosQuadPixelsFailed : kXenosQuadPassed,'''),
    ('''  _quadMipHash = _quadSampleHash = XENOS_QUAD_HASH_INITIAL;''',
     '''  const bool trilinear = _quadGeometryMode == kXenosQuadUploadTrilinearBatch;
  const char *log = trilinear ? "XenosBatchTrilinear" : "XenosBatchMipmap";
  _quadMipHash = _quadSampleHash = XENOS_QUAD_HASH_INITIAL;'''),
    ('''          IOLog("XenosBatchMipmap: tail mismatch=(%lu,%lu) expected=%08lx observed=%08lx\\n",
                x, y, expectedMip, mipPixel);''',
     '''          IOLog("%s: tail mismatch=(%lu,%lu) expected=%08lx observed=%08lx\\n",
                log, x, y, expectedMip, mipPixel);'''),
    ('      _quadMipHash = quadHashColor(_quadMipHash, mipPixel);',
     '''      _quadMipHash = quadHashColor(_quadMipHash, mipPixel);
      if (trilinear)
        continue;'''),
    ('  _quadMipChecked = _quadSampleChecked = kClearPixels;',
     '''  if (trilinear)
    verifyQuadTrilinearSamples(base, mip, sample);
  _quadMipChecked = _quadSampleChecked = kClearPixels;'''),
    ('''  if (_quadGeometryMode == kXenosQuadUploadMipmapBatch) {
    setProperty''',
     '''  if (_quadGeometryMode == kXenosQuadUploadTrilinearBatch) {
    setProperty("XenosBatchTrilinearWitnesses", _quadTrilinearWitnesses, 32);
    setProperty("XenosBatchTrilinearMaxDelta", _quadTrilinearMaxDelta, 32);
    IOLog("XenosBatchTrilinear: %s ticket=%lu fence=%lu first_pixels=%lu first_mismatches=%lu "
          "mip_pixels=%lu mip_mismatches=%lu sample_pixels=%lu sample_mismatches=%lu "
          "level0_pixels=%lu blended_pixels=%lu level1_pixels=%lu guard_pixels=%lu blend_evidence=%lu "
          "max_delta=%lu first_hash=%08lx mip_hash=%08lx sample_hash=%08lx\\n",
          result, _quadTicket, fence, checked - _quadMipChecked - _quadSampleChecked,
          mismatches - _quadMipMismatches - _quadSampleMismatches, _quadMipChecked,
          _quadMipMismatches, _quadSampleChecked, _quadSampleMismatches, _quadMipBaseMatches,
          _quadTrilinearMatches, _quadMipLevel1Matches, _quadSampleGuard, _quadTrilinearWitnesses,
          _quadTrilinearMaxDelta, hash, _quadMipHash, _quadSampleHash);
  } else if (_quadGeometryMode == kXenosQuadUploadMipmapBatch) {
    setProperty'''),
    ('''    const UInt32 mipSampleEnd = kMipSampleOrigin + kMipSide;''',
     '''    packQuadSampleQuad(sampleGeometrySlot, buildMip, -1, -1);
    if (trilinear) {
      for (UInt32 index = 0; index < kTrilinearRegionCount; ++index) {
        const UInt32 left = kTrilinearRegionX[index];
        const XenosQuadGeometry region = xenosQuadMakeGeometry(
            left, kTrilinearRegionY, left + kTrilinearWidth, kTrilinearRegionY + kTrilinearHeight,
            kTrilinearU, kTrilinearV, kTrilinearU + kTrilinearWidth, kTrilinearV + kTrilinearHeight);
        packQuadSampleQuad(sampleGeometrySlot + (index + 1) * kQuadGeometryStride, region,
                           kTrilinearUQuarter, kTrilinearVQuarter);
      }
    } else {
    const UInt32 mipSampleEnd = kMipSampleOrigin + kMipSide;'''),
    ('''    packQuadSampleQuad(sampleGeometrySlot, buildMip, -1, -1);
    packQuadSampleQuad(sampleGeometrySlot + kQuadGeometryStride, sampleMip, 1, 1);
    packQuadSampleQuad(sampleGeometrySlot + 2 * kQuadGeometryStride, sampleBase, 0, 0);''',
     '''    packQuadSampleQuad(sampleGeometrySlot + kQuadGeometryStride, sampleMip, 1, 1);
    packQuadSampleQuad(sampleGeometrySlot + 2 * kQuadGeometryStride, sampleBase, 0, 0);
    }'''),
    ('  const UInt32 sampleQuads = mipmap ? 3 : renderTexture ? 1 : 0;',
     '''  const UInt32 sampleQuads = trilinear ? 1 + kTrilinearRegionCount :
                             mipmap ? 3 : renderTexture ? 1 : 0;'''),
    ('''    // Both draws use the same texture and sampler. Their screen size
    // selects level 1 at 2:1 minification and level 0 at 1:1.
    clearQuadSampleTarget(base);
    beginQuadSampleDraw();
    for (UInt32 position = buildPosition + kQuadGeometryStride;
         position <= buildPosition + 2 * kQuadGeometryStride; position += kQuadGeometryStride)
      drawTextureLayer(position, position + CACHELINE_SIZE, readback, SQ_TEX_FILTER_POINT, mip);''',
     '''    clearQuadSampleTarget(base);
    beginQuadSampleDraw();
    if (trilinear) {
      // All three draws have the same 1:1 UV gradients. Bias selects
      // LOD 0, 0.5, and 1 with bilinear sampling within each level.
      for (UInt32 index = 0; index < kTrilinearRegionCount; ++index) {
        const UInt32 position = buildPosition + (index + 1) * kQuadGeometryStride;
        drawTextureLayer(position, position + CACHELINE_SIZE, readback, SQ_TEX_FILTER_BILINEAR, mip,
                         SQ_TEX_FILTER_BILINEAR, kTrilinearLodSteps[index]);
      }
    } else {
      // Screen size selects level 1 at 2:1 minification and level 0 at 1:1.
      for (UInt32 position = buildPosition + kQuadGeometryStride;
           position <= buildPosition + 2 * kQuadGeometryStride; position += kQuadGeometryStride)
        drawTextureLayer(position, position + CACHELINE_SIZE, readback, SQ_TEX_FILTER_POINT, mip);
    }'''),
    ('''enum sq_tex_filter filter, UInt32 mipAddress) {
  setGpuVertices(positions, 12 * sizeof(UInt32), uv, 8 * sizeof(UInt32));
  setGpuTexture(texture, kClearSide, kClearSide, filter, true, mipAddress);''',
     '''enum sq_tex_filter filter, UInt32 mipAddress,
                                         enum sq_tex_filter mipFilter, UInt32 lodBiasSteps) {
  setGpuVertices(positions, 12 * sizeof(UInt32), uv, 8 * sizeof(UInt32));
  setGpuTexture(texture, kClearSide, kClearSide, filter, true, mipAddress, mipFilter, lodBiasSteps);'''),
])
text = controller.read_text()
begin = text.index('    const UInt32 mipSampleEnd')
end = text.index('\n    }', begin)
text = text[:begin] + '\n'.join('  ' + line for line in text[begin:end].split('\n')) + text[end:]
controller.write_text(text)

app = root / 'app/XenosQuadBatchMipmap.c'
target = app.with_name('XenosQuadBatchTrilinear.c')
text = app.read_text().replace('XenosQuadBatchMipmap', 'XenosQuadBatchTrilinear')
text = text.replace('kXenosQuadUploadMipmapBatch', 'kXenosQuadUploadTrilinearBatch')
text = text.replace('Boot the quad batch mipmap test', 'Boot the quad batch trilinear test')
text = text.replace('Mipmap batch upload failed', 'Trilinear batch upload failed')
text = text.replace('mip_levels=2 batch_draws=', 'mip_levels=2 lods=0,0.5,1 batch_draws=')
text = text.replace('and both regions', 'and all three regions')
target.write_text(text)
app.unlink()
