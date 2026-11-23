from pathlib import Path

root = Path(__file__).resolve().parent / "source/XenonCoreGraphics"
make = root / "Makefile"
make.write_text(make.read_text().replace("0.0.5d13", "0.0.5d14"))
header = root / "src/XenosController.hpp"
s = header.read_text().replace("  UInt32                    _movingWait;", """  UInt32                    _movingWait;
  UInt32                    _vblankWaitUS;
  UInt32                    _vblankHeight;
  UInt32                    _vblankHTotal;
  UInt32                    _vblankVTotal;
  UInt32                    _frontIndex;
  UInt32                    _bufferFlips[2];
  UInt32                    _orderErrors;
  UInt32                    _lateFlips;
  UInt32                    _lastFlipLine;
  bool                      _needActiveScan;
  bool                      _frameReady;
  bool                      _desktopRestored;
  IOBufferMemoryDescriptor  *_alternateBuffer;
  UInt32                    _alternatePhysical;""")
s = s.replace("  bool stepMovingComposite();", """  enum FlipResult { kFlipRetry, kFlipComplete, kFlipLate, kFlipReadbackFailed };
  UInt32 stepMovingComposite();
  UInt32 nextVBlankPollUS();
  bool scanoutAt(UInt32 address);
  bool displayModeMatches();
  bool restoreDesktopScanout(bool drained);
  FlipResult flipDuringVBlank(UInt32 address, UInt32 activeHeight);
  UInt32 scanoutAddress(UInt32 index) const;
  volatile UInt32 *scanoutBuffer(UInt32 index);
  bool checkFrameNumber(UInt32 index, UInt32 frame);""")
s = s.replace("  bool movingScanoutMatches();\n", "")
s = s.replace("  void submitMovingComposite();", "  void submitMovingComposite(UInt32 target);")
s = s.replace("  void checkMovingComposite(UInt32 frame);", "  void checkMovingComposite(volatile UInt32 *scanout, UInt32 frame);")
header.write_text(s)

source = root / "src/XenosController.cpp"
s = source.read_text().replace("#include <IOKit/IOPlatformExpert.h>", """#include <IOKit/IOPlatformExpert.h>
#include <ppc/machine_routines.h>""")
s = s.replace("static const UInt32 kMovingFrames = 960;", "static const UInt32 kMovingFrames = 480;")
s = s.replace("static const UInt32 kMovingWaitLimit = 16;", """static const UInt32 kRefreshUS = 15000;
static const UInt32 kFencePollUS = 1000;
static const UInt32 kMovingWaitLimit = 240; // Fence polls, approximately 240 ms.
static const UInt32 kVBlankWaitLimitUS = 250000;
static const UInt32 kVBlankPollUS = 120;
static const UInt32 kEarlyBlankLines = 12;
static const UInt32 kApplyPolls = 32;""")
s = s.replace("static const UInt32 kXenosRegD1GrphPitch = 0x6120;", """static const UInt32 kXenosRegD1GrphPitch = 0x6120;
static const UInt32 kXenosRegD1GrphEndY = 0x6138;
static const UInt32 kXenosRegDisplayBase = 0x2840;
static const UInt32 kXenosRegCrtcHTotal = 0x6000;
static const UInt32 kXenosRegCrtcVTotal = 0x6010;
static const UInt32 kXenosRegCrtcUpdate = 0x60e8;
static const UInt32 kXenosRegCrtcApply = 0x60ec;
static const UInt32 kXenosRegGraphicsUpdate = 0x6144;
static const UInt32 kXenosRegScanline = 0x6530;
static const UInt32 kXenosRegScanStatus = 0x6534;
static const UInt32 kXenosRegScalerUpdate = 0x65cc;
static const UInt32 kVBlankActive = 1U << 12;
static const UInt32 kScanlineMask = 0x1fff;""")
start = s.index("static UInt32 backTexel")
s = s[:start] + """// The top strip is outside the motion and blend probes. Two opaque texels
// encode the GPU-rendered frame number and its complement per RGB channel.
static UInt32 frameColor(UInt32 frame) {
  return 0xff800000U | frame;
}

static UInt32 frameCheckColor(UInt32 frame) {
  return frameColor(frame) ^ 0x00ffffffU;
}

""" + s[start:]
s = s.replace("""  _movingWait = 0;
  _movingSubmitted""", """  _movingWait = 0;
  _vblankWaitUS = 0;
  _frontIndex = 0;
  _bufferFlips[0] = _bufferFlips[1] = 0;
  _orderErrors = _lateFlips = 0;
  _lastFlipLine = 0;
  _needActiveScan = _frameReady = _desktopRestored = false;
  _alternateBuffer = NULL;
  _alternatePhysical = 0;
  _movingSubmitted""", 1)
s = s.replace("""  setProperty("XenosMovingCompositeResult", "Pending");""", """  // A coherent full-size alternate surface needs one physical segment.
  // Allocate before the system has populated its normal caches.
  if (((_fbWidth == 1280 && _fbHeight == 720) ||
       (_fbWidth == 1920 && _fbHeight == 1080)) &&
      _gpuPhysAddr == kDisplayMemoryStart + kXenosFramebufferLength) {
    const UInt32 capacity = _fbWidth * ALIGN(_fbHeight, 32) * sizeof(UInt32);
    IOBufferMemoryDescriptor *buffer = IOBufferMemoryDescriptor::withOptions(
        kIOMemoryPhysicallyContiguous, capacity, PAGE_SIZE);
    IOByteCount length = 0;
    const IOPhysicalAddress address = buffer ? buffer->getPhysicalSegment(0, &length) : 0;
    if (address && !(address & (PAGE_SIZE - 1)) &&
        length >= capacity && address <= kDisplayMemoryStart - capacity &&
        buffer->getBytesNoCopy()) {
      _alternateBuffer = buffer;
      _alternatePhysical = static_cast<UInt32>(address);
    } else if (buffer) {
      buffer->release();
    }
  }
  setProperty("XenosVBlankFlipResult", "Pending");""")
start = s.index("  if (_movingDelay)\n")
end = s.index("void XenosController::drawQuad()", start)
s = s[:start] + """  if (_movingDelay)
    --_movingDelay;
  UInt32 interval = kRefreshUS;
  // Keep the batch intact if the marker timed out. A stalled GPU may still
  // own its scratch page.
  if (!_markerScratch || readReg32(kXenosRegCpRbReadPtr) == _ringBufferWritePtr) {
    const UInt32 diagnosticInterval = _movingDelay ? 0 : stepMovingComposite();
    if (diagnosticInterval) {
      interval = diagnosticInterval;
    } else {
      setGpuEdRamMode(COLOR_DEPTH);
      drawQuad();
      setGpuEdRamMode(EDRAM_COPY);
      drawResolveRectangle();
      syncRingBuffer();
    }
  }
  sender->setTimeoutUS(interval);
}

""" + s[end:]
start = s.index("bool XenosController::movingScanoutMatches()")
end = s.index("  IODeviceMemory *surface", start)
s = s[:start] + """bool XenosController::scanoutAt(UInt32 address) {
  return readReg32(kXenosRegD1GrphPriSurfaceAddr) == address &&
         readReg32(kXenosRegDisplayBase) == address;
}

bool XenosController::displayModeMatches() {
  return readReg32(kXenosRegD1GrphPitch) == _fbWidth &&
         readReg32(kXenosRegD1GrphEndY) == _vblankHeight &&
         (readReg32(kXenosRegCrtcHTotal) & kScanlineMask) == _vblankHTotal &&
         (readReg32(kXenosRegCrtcVTotal) & kScanlineMask) == _vblankVTotal;
}

UInt32 XenosController::scanoutAddress(UInt32 index) const {
  return index ? _alternatePhysical : _gpuPhysAddr;
}

volatile UInt32 *XenosController::scanoutBuffer(UInt32 index) {
  if (index)
    return static_cast<volatile UInt32 *>(_alternateBuffer->getBytesNoCopy());
  return reinterpret_cast<volatile UInt32 *>(_movingScanout->getVirtualAddress());
}

bool XenosController::initMovingComposite() {
  if (!_alternateBuffer) {
    finishMovingComposite("SKIP", "no contiguous second scanout surface");
    return false;
  }
  _vblankHeight = readReg32(kXenosRegD1GrphEndY);
  _vblankHTotal = readReg32(kXenosRegCrtcHTotal) & kScanlineMask;
  _vblankVTotal = readReg32(kXenosRegCrtcVTotal) & kScanlineMask;
  const bool native720 = _vblankHeight == 720 && _vblankHTotal == 1649 && _vblankVTotal == 749;
  const bool native1080 = _vblankHeight == 1080 && _vblankHTotal == 2199 && _vblankVTotal == 1124;
  if ((!native720 && !native1080) || !scanoutAt(_gpuPhysAddr) || !displayModeMatches()) {
    finishMovingComposite("SKIP", "unsupported scanout surface, pitch, or timing");
    return false;
  }
""" + s[end:]
start = s.index('  IOLog("XenosMovingComposite: START')
end = s.index("  UInt8 *memory = static_cast<UInt8 *>(_movingScratch->getBytesNoCopy());",
              s.index("void XenosController::submitMovingComposite()", start))
s = s[:start] + """  IOLog("XenosVBlankFlip: START render=%lux%lu active_height=%lu frames=%lu buffers=0x%08lx,0x%08lx\\n",
        _fbWidth, _fbHeight, _vblankHeight, kMovingFrames, _gpuPhysAddr, _alternatePhysical);
  setProperty("XenosVBlankFlipResult", "Running");
  return true;
}

UInt32 XenosController::nextVBlankPollUS() {
  const UInt32 line = readReg32(kXenosRegScanline) & kScanlineMask;
  const UInt32 height = readReg32(kXenosRegD1GrphEndY);
  if (line >= height)
    return 1000;
  // Wake close to blanking, then poll in short intervals on the work loop.
  // Use the shorter of the 720p and 1080p line periods for the estimate.
  const UInt32 remaining = height - line;
  if (remaining > 24) {
    const UInt32 delay = (remaining - 16) * 14;
    return delay < 4000 ? delay : 4000;
  }
  return kVBlankPollUS;
}

XenosController::FlipResult XenosController::flipDuringVBlank(UInt32 address,
                                                            UInt32 activeHeight) {
  if (!(readReg32(kXenosRegScanStatus) & kVBlankActive)) {
    _needActiveScan = false;
    return kFlipRetry;
  }
  if (_needActiveScan)
    return kFlipRetry;

  // Keep the flag check and address commit together on the same CPU.
  const boolean_t interrupts = ml_set_interrupts_enabled(false);
  const UInt32 before = readReg32(kXenosRegScanline) & kScanlineMask;
  if (!(readReg32(kXenosRegScanStatus) & kVBlankActive) ||
      before < activeHeight || before >= activeHeight + kEarlyBlankLines ||
      readReg32(kXenosRegCrtcUpdate) || readReg32(kXenosRegCrtcApply) ||
      (readReg32(kXenosRegGraphicsUpdate) & 1) ||
      (readReg32(kXenosRegScalerUpdate) & 0x10000)) {
    ml_set_interrupts_enabled(interrupts);
    return kFlipRetry;
  }

  writeReg32(kXenosRegDisplayBase, address);
  writeReg32(kXenosRegD1GrphPriSurfaceAddr, address);
  writeReg32(kXenosRegCrtcApply, 1);
  UInt32 pending = readReg32(kXenosRegCrtcApply);
  for (UInt32 attempt = 0; pending && attempt < kApplyPolls; ++attempt)
    pending = readReg32(kXenosRegCrtcApply);
  const bool acknowledged = !pending && scanoutAt(address);
  const UInt32 after = readReg32(kXenosRegScanline) & kScanlineMask;
  const bool inBlank = (readReg32(kXenosRegScanStatus) & kVBlankActive) &&
                       after >= before && after >= activeHeight;
  ml_set_interrupts_enabled(interrupts);
  _needActiveScan = true;
  _lastFlipLine = after;
  if (!acknowledged)
    return kFlipReadbackFailed;
  return inBlank ? kFlipComplete : kFlipLate;
}

bool XenosController::restoreDesktopScanout(bool drained) {
  if (_desktopRestored)
    return true;
  if (!drained)
    return false;

  if (!scanoutAt(_gpuPhysAddr) || readReg32(kXenosRegCrtcApply)) {
    // Only restore our own scanout. Leave changes by another display owner alone.
    const UInt32 primary = readReg32(kXenosRegD1GrphPriSurfaceAddr);
    const UInt32 base = readReg32(kXenosRegDisplayBase);
    if (!_alternateBuffer ||
        (primary != _gpuPhysAddr && primary != _alternatePhysical) ||
        (base != _gpuPhysAddr && base != _alternatePhysical))
      return false;
    const UInt32 height = readReg32(kXenosRegD1GrphEndY);
    const FlipResult restore = flipDuringVBlank(_gpuPhysAddr, height);
    if ((restore != kFlipComplete && restore != kFlipLate) || !scanoutAt(_gpuPhysAddr))
      return false;
    IOLog("XenosVBlankFlip: desktop scanout restored at line %lu%s\\n",
          _lastFlipLine, restore == kFlipLate ? " (late)" : "");
  }

  if (_movingScanout) {
    _movingScanout->release();
    _movingScanout = NULL;
  }
  // If the fence never landed, the GPU may still reference both allocations.
  if (_movingSubmitted == _movingChecked || _frameReady) {
    if (_movingScratch) {
      _movingScratch->release();
      _movingScratch = NULL;
    }
    if (_alternateBuffer) {
      _alternateBuffer->release();
      _alternateBuffer = NULL;
    }
  }
  _desktopRestored = true;
  setProperty("XenosVBlankFlipDesktopRestored", true);
  return true;
}

UInt32 XenosController::stepMovingComposite() {
  const bool drained = readReg32(kXenosRegCpRbReadPtr) == _ringBufferWritePtr &&
                       readReg32(kXenosRegCpRbWritePtr) == _ringBufferWritePtr;
  if (_movingFinished)
    return restoreDesktopScanout(drained) ? 0 : nextVBlankPollUS();

  if (!_movingScratch) {
    if (!drained)
      return kFencePollUS;
    if (!initMovingComposite())
      return restoreDesktopScanout(drained) ? 0 : kRefreshUS;
  }
  if (!displayModeMatches() || !scanoutAt(scanoutAddress(_frontIndex))) {
    finishMovingComposite("FAIL", "display changed during the test");
    return kFencePollUS;
  }

  const UInt32 backIndex = 1 - _frontIndex;
  if (_movingSubmitted == _movingChecked) {
    if (!drained)
      return kFencePollUS;
    if (_ringBufferWritePtr > kXenosRingbufferLength - kMovingRingDwords) {
      // Keep each batch contiguous. Type-2 NOPs take us to ring slot zero.
      while (_ringBufferWritePtr)
        writeRing(2U << 30);
      syncRingBuffer();
      return kFencePollUS;
    }
    _movingWait = _vblankWaitUS = 0;
    _frameReady = false;
    submitMovingComposite(scanoutAddress(backIndex));
    return kFencePollUS;
  }

  if (!_frameReady) {
    UInt8 *memory = static_cast<UInt8 *>(_movingScratch->getBytesNoCopy());
    volatile UInt32 *fence = reinterpret_cast<volatile UInt32 *>(memory + 3 * PAGE_SIZE);
    if (drained) {
      flushMarkerCacheLine(fence);
      _frameReady = *fence == _movingSubmitted;
    }
    if (!_frameReady) {
      if (++_movingWait >= kMovingWaitLimit)
        finishMovingComposite("FAIL", drained ? "GPU completion marker missing" : "GPU ring did not drain");
      return kFencePollUS;
    }
    // The active surface must still contain the previous frame after rendering
    // into the other surface. Its serial is independent of position and alpha.
    if (_movingChecked && !checkFrameNumber(_frontIndex, _movingChecked)) {
      finishMovingComposite("FAIL", "front buffer changed before flip");
      return kFencePollUS;
    }
  }

  const FlipResult flip = flipDuringVBlank(scanoutAddress(backIndex), _vblankHeight);
  if (flip == kFlipRetry) {
    const UInt32 delay = nextVBlankPollUS();
    _vblankWaitUS += delay;
    if (_vblankWaitUS > kVBlankWaitLimitUS)
      finishMovingComposite("FAIL", "timed out waiting for VBlank");
    return delay;
  }
  if (flip != kFlipComplete) {
    _lateFlips += flip == kFlipLate;
    finishMovingComposite("FAIL", flip == kFlipLate ? "flip crossed out of VBlank" : "scanout address did not latch");
    return kFencePollUS;
  }

  _frontIndex = backIndex;
  ++_bufferFlips[_frontIndex];
  if (!checkFrameNumber(_frontIndex, _movingSubmitted)) {
    finishMovingComposite("FAIL", "displayed frame number out of order");
    return kFencePollUS;
  }
  checkMovingComposite(scanoutBuffer(_frontIndex), _movingChecked);
  ++_movingChecked;
  if (_movingChecked == kMovingFrames) {
    const bool passed = !_movingMismatches && !_orderErrors && !_lateFlips && _movingOpaque &&
                        _movingLow && _movingHigh && _movingChanges == kMovingFrames - 1 &&
                        _bufferFlips[0] == kMovingFrames / 2 && _bufferFlips[1] == kMovingFrames / 2;
    finishMovingComposite(passed ? "PASS" : "FAIL", "frame checks complete");
    return restoreDesktopScanout(true) ? 0 : kFencePollUS;
  }
  return kFencePollUS;
}

void XenosController::submitMovingComposite(UInt32 target) {
""" + s[end:]
s = s.replace("""  syncData(memory, 2 * CACHELINE_SIZE);

  const UInt32 savedCoherDest""", """  syncData(memory, 2 * CACHELINE_SIZE);

  UInt32 *back = reinterpret_cast<UInt32 *>(memory + PAGE_SIZE);
  back[resolveTileWord(0, 0)] = frameColor(_movingSubmitted + 1);
  back[resolveTileWord(2, 0)] = frameCheckColor(_movingSubmitted + 1);
  syncData(back, CACHELINE_SIZE);

  const UInt32 savedCoherDest""")
s = s.replace("""    syncGpuMemory(_movingPhysical, PAGE_SIZE, kCoherVertexCache);""", """    syncGpuMemory(_movingPhysical, 2 * PAGE_SIZE, kCoherVertexAndTextureCaches);""")
s = s.replace("""  // Restore the same state used by the regular refresh before resolving to scanout.""",
              """  // Restore the regular refresh state before resolving into the hidden surface.""")
s = s.replace("setResolveTarget(_gpuPhysAddr, _fbWidth, _fbHeight, kResolveConvert, kResolveDisplayFormat);",
              "setResolveTarget(target, _fbWidth, _fbHeight, kResolveConvert, kResolveDisplayFormat);", 1)
s = s.replace("""  const UInt32 top = origins.backY & ~31U;
  const UInt32 bottom = ALIGN(origins.backY + kLayerSide, 32);
  syncGpuMemory(_gpuPhysAddr + top * _fbWidth * sizeof(UInt32),
                (bottom - top) * _fbWidth * sizeof(UInt32), kCoherResolveActions);""", """  syncGpuMemory(target, _fbWidth * ALIGN(_fbHeight, 32) * sizeof(UInt32), kCoherResolveActions);""")
s = s.replace("""  writeRingReg32(4 * kXenosCoherDestBase1, savedCoherDest);
  writeRingPacket3(CP_MEM_WRITE, 2);
  writeRing((_movingPhysical""", """  writeRingReg32(4 * kXenosCoherDestBase1, savedCoherDest);
  // Normal refresh can resume on buffer A without inheriting the hidden target.
  setResolveTarget(_gpuPhysAddr, _fbWidth, _fbHeight, kResolveConvert, kResolveDisplayFormat);
  writeRingPacket3(CP_MEM_WRITE, 2);
  writeRing((_movingPhysical""")
start = s.index("void XenosController::checkMovingComposite(UInt32 frame)")
end = s.index("  UInt32 frameProbes", start)
s = s[:start] + """bool XenosController::checkFrameNumber(UInt32 index, UInt32 frame) {
  const LayerOrigins origins = movingLayerOrigins(frame - 1, _fbWidth, _fbHeight);
  volatile UInt32 *surface = scanoutBuffer(index);
  const UInt32 word = readScanoutPixel(surface, _fbWidth, origins.backX + 1, origins.backY + 1);
  const UInt32 check = readScanoutPixel(surface, _fbWidth, origins.backX + 9, origins.backY + 1);
  if (word == frameColor(frame) && check == frameCheckColor(frame))
    return true;
  ++_orderErrors;
  IOLog("XenosVBlankFlip: serial mismatch buffer=%lu expected=%lu word=0x%08lx check=0x%08lx\\n",
        index, frame, word, check);
  return false;
}

void XenosController::checkMovingComposite(volatile UInt32 *scanout, UInt32 frame) {
  const LayerOrigins origins = movingLayerOrigins(frame, _fbWidth, _fbHeight);
""" + s[end:]
s = s.replace("""  if ((frame + 1) % 240 == 0) {
    IOLog("XenosMovingComposite: frame=%lu back_x=%lu front_x=%lu probes=%lu mismatches=%lu\\n",
          frame + 1, origins.backX, origins.frontX, frameProbes, _movingMismatches);
  }""", """  if (frame < 4 || (frame + 1) % 120 == 0) {
    IOLog("XenosVBlankFlip: frame=%lu buffer=%lu vblank_line=%lu probes=%lu mismatches=%lu\\n",
          frame + 1, _frontIndex, _lastFlipLine, frameProbes, _movingMismatches);
  }""")
start = s.index("  if (_movingScanout)", s.index("void XenosController::finishMovingComposite"))
end = s.index("void XenosController::drawTextureLayer", start)
s = s[:start] + """  setProperty("XenosVBlankFlipFrames", _movingChecked, 32);
  setProperty("XenosVBlankFlipBufferA", _bufferFlips[0], 32);
  setProperty("XenosVBlankFlipBufferB", _bufferFlips[1], 32);
  setProperty("XenosVBlankFlipMovedFrames", _movingChanges, 32);
  setProperty("XenosVBlankFlipLowAlphaProbes", _movingLow, 32);
  setProperty("XenosVBlankFlipHighAlphaProbes", _movingHigh, 32);
  setProperty("XenosVBlankFlipPixelMismatches", _movingMismatches, 32);
  setProperty("XenosVBlankFlipOrderErrors", _orderErrors, 32);
  setProperty("XenosVBlankFlipLate", _lateFlips, 32);
  setProperty("XenosVBlankFlipResult", result);
  IOLog("XenosVBlankFlip: %s %s checked=%lu/%lu A=%lu B=%lu order_errors=%lu late=%lu "
        "moved=%lu opaque=%lu alpha64=%lu alpha192=%lu mismatches=%lu\\n",
        result, reason, _movingChecked, kMovingFrames, _bufferFlips[0], _bufferFlips[1],
        _orderErrors, _lateFlips, _movingChanges, _movingOpaque, _movingLow, _movingHigh,
        _movingMismatches);
}

""" + s[end:]
s = s.replace("XenosMovingComposite:", "XenosVBlankFlip:")
source.write_text(s)
