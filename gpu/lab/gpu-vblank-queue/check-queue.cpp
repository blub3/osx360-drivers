
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
typedef unsigned int UInt32;
typedef unsigned short UInt16;
typedef unsigned char UInt8;
typedef bool boolean_t;
#define PAGE_SIZE 4096
#define kXenosRegD1GrphPriSurfaceAddr 0x6110
#define kXenosRegCpRbReadPtr 0x710
#define kXenosRegCpRbWritePtr 0x714
#define kXenosRingbufferLength 8192
bool ml_set_interrupts_enabled(bool enabled) { return !enabled; }
void OSSynchronizeIO() {}
void flushMarkerCacheLine(volatile UInt32 *) {}
static const UInt32 kMovingDelay = 1200; // About 18 seconds of normal display refreshes.
static const UInt32 kMovingFrames = 480;
static const UInt32 kRefreshUS = 15000;
static const UInt32 kFencePollUS = 1000;
static const UInt32 kMovingWaitLimit = 240; // Fence polls, approximately 240 ms.
static const UInt32 kVBlankWaitLimitUS = 250000;
static const UInt32 kVBlankPollUS = 120;
static const UInt32 kEarlyBlankLines = 16;
static const UInt32 kApplyPolls = 32;
static const UInt32 kMovingRingDwords = 256;
static const UInt32 kTextureSide = 32;
static const UInt32 kTextureScale = 4;
static const UInt32 kLayerSide = kTextureSide * kTextureScale;
static const UInt32 kTravel = 64;
static const UInt32 kPixelsPerFrame = 4;
static const UInt32 kScanoutTileSide = 32;
static const UInt32 kLayerYOffset = 32;
static const UInt16 kXenosWaitUntil = 0x05c8;
static const UInt32 kWait3dIdleClean = 1 << 17;
static const UInt16 kXenosCoherDestBase1 = 0x2007;
static const UInt16 kXenosCoherSizeHost = 0x0a2f;
static const UInt16 kXenosCoherStatusHost = 0x0a31;
static const UInt32 kXenosRegD1GrphPitch = 0x6120;
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
static const UInt32 kScanlineMask = 0x1fff;
static const UInt32 kCoherVertexCache = 1 << 24;
static const UInt32 kCoherVertexAndTextureCaches = kCoherVertexCache | (1 << 25);
static const UInt32 kCoherResolveActions = kCoherVertexAndTextureCaches | 0x00000100;
static const UInt32 kCoherBusy = 1U << 31;
static const UInt32 kCacheFlushEvent = 6;
static const int kBlendTolerance = 1;
static const UInt32 kLowAlpha = 64;
static const UInt32 kHighAlpha = 192;

struct LayerOrigins {
  UInt32 backX, backY;
  UInt32 frontX, frontY;
};

static LayerOrigins movingLayerOrigins(UInt32 frame, UInt32 width, UInt32 height) {
  const UInt32 step = (frame * kPixelsPerFrame) % (2 * kTravel);
  const UInt32 travel = step <= kTravel ? step : 2 * kTravel - step;
  const UInt32 left = (width - kLayerSide - kTravel) / 2;
  const UInt32 top = (height - kLayerSide - kLayerYOffset) / 2;
  const LayerOrigins origins = {
    left + travel, top, left + kTravel - travel, top + kLayerYOffset
  };
  return origins;
}

static UInt32 frameColor(UInt32 frame) {
  return 0xff800000U | frame;
}

static UInt32 frameCheckColor(UInt32 frame) {
  return frameColor(frame) ^ 0x00ffffffU;
}


class XenosController {
public:
    enum FlipResult { kFlipRetry, kFlipComplete, kFlipLate, kFlipReadbackFailed };
    UInt32 regs[0x8000]{};
    UInt32 _vblankHeight=1080, _frontIndex=0, _movingSubmitted=0, _movingChecked=0,
           _ringBufferWritePtr=0, _lateFlips=0, _latchErrors=0, _lastFlipLine=0,
           _movingWait=0, _vblankWaitUS=0, _movingChanges=0, _movingMismatches=0,
           _orderErrors=0, _movingOpaque=1, _movingLow=1, _movingHigh=1;
    
    enum { kScanoutCount = 3, kReadyCapacity = 2, kNoBuffer = 3 };
    struct QueuedFrame { UInt32 number, index; };
    UInt32 _bufferFlips[3]{}, frames[3]{}, _renderIndex=kNoBuffer,
           _readyCount=0, _peakReady=0, _framesQueued=0, _twoDeepFlips=0;
    QueuedFrame _ready[kReadyCapacity]{};
    bool _queuePrimed=false;
    UInt32 freeScanoutIndex() const;
    bool collectRenderedFrame(bool);
    bool presentQueuedFrame();
    bool _movingFinished=false, _needActiveScan=false, _frameReady=false, _flipAttempted=false;
    bool blank=false, simulatedLate=false, simulatedLatchFailure=false, restored=false, passed=false;
    const char *failure=nullptr;
    struct Scratch {
        UInt8 data[4*PAGE_SIZE]{};
        void *getBytesNoCopy() { return data; }
    } scratch, *_movingScratch=&scratch;

    XenosController() {
        regs[kXenosRegDisplayBase]=regs[kXenosRegD1GrphPriSurfaceAddr]=scanoutAddress(0);
    }
    UInt32 readReg32(UInt32 offset) {
        if (offset==kXenosRegScanStatus)
            return blank ? kVBlankActive : 0;
        if (offset==kXenosRegScanline)
            return blank ? 1082 : 500;
        return regs[offset];
    }
    void writeReg32(UInt32 offset, UInt32 value) {
        regs[offset]=value;
        if (offset==kXenosRegCrtcApply) {
            if (!simulatedLatchFailure)
                regs[offset]=0;
            if (simulatedLate)
                blank=false;
        }
    }
    bool scanoutAt(UInt32 address) {
        return readReg32(kXenosRegDisplayBase)==address && readReg32(kXenosRegD1GrphPriSurfaceAddr)==address;
    }
    UInt32 scanoutAddress(UInt32 index) { return index ? 0x1e000000 + (index-1)*0x800000 : 0x1f800000; }
    bool restoreDesktopScanout(bool drained) {
        restored=scanoutAt(scanoutAddress(0)) && drained;
        return restored;
    }
    bool initMovingComposite() { return true; }
    bool displayModeMatches() { return true; }
    UInt32 nextVBlankPollUS() { return 1000; }
    void writeRing(UInt32) { _ringBufferWritePtr=0; }
    void syncRingBuffer() {}
    void submitMovingComposite(UInt32 address) {
        assert(_renderIndex != kNoBuffer);
        assert(!scanoutAt(address) && address == scanoutAddress(_renderIndex));
        for (unsigned i=0; i<_readyCount; ++i)
            assert(_ready[i].index != _renderIndex);
        ++_movingSubmitted;
        regs[kXenosRegCpRbWritePtr]=++_ringBufferWritePtr;
    }
    void completeFrame(bool stale=false) {
        assert(!scanoutAt(scanoutAddress(_renderIndex)));
        frames[_renderIndex]=_movingSubmitted-stale;
        regs[kXenosRegCpRbReadPtr]=_ringBufferWritePtr;
        *reinterpret_cast<UInt32 *>(scratch.data+3*PAGE_SIZE)=_movingSubmitted;
    }
    bool checkFrameNumber(UInt32 index, UInt32 frame) {
        const bool ok=frame==frames[index];
        _orderErrors+=!ok;
        return ok;
    }
    volatile UInt32 *scanoutBuffer(UInt32 index) { return &frames[index]; }
    void checkMovingComposite(volatile UInt32 *scanout, UInt32 frame) {
        assert(*scanout==frame+1);
        _movingChanges+=(frame!=0);
    }
    void finishMovingComposite(const char *result, const char *reason) {
        _movingFinished=true;
        passed=!std::strcmp(result,"PASS");
        failure=reason;
    }
    FlipResult flipDuringVBlank(UInt32 address, UInt32 height);
    UInt32 stepMovingComposite();
};
UInt32 XenosController::freeScanoutIndex() const {
  for (UInt32 index = 0; index < kScanoutCount; ++index) {
    if (index == _frontIndex || index == _renderIndex)
      continue;
    bool queued = false;
    for (UInt32 entry = 0; entry < _readyCount; ++entry)
      queued |= _ready[entry].index == index;
    if (!queued)
      return index;
  }
  return kNoBuffer;
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
      (readReg32(kXenosRegCrtcUpdate) & 1) || readReg32(kXenosRegCrtcApply) ||
      (readReg32(kXenosRegGraphicsUpdate) & 1) ||
      (readReg32(kXenosRegScalerUpdate) & 0x10000)) {
    ml_set_interrupts_enabled(interrupts);
    return kFlipRetry;
  }

  _flipAttempted = true;
  writeReg32(kXenosRegDisplayBase, address);
  OSSynchronizeIO();
  writeReg32(kXenosRegD1GrphPriSurfaceAddr, address);
  OSSynchronizeIO();
  writeReg32(kXenosRegCrtcApply, 1);
  OSSynchronizeIO();
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
  _lateFlips += !inBlank;
  _latchErrors += !acknowledged;
  if (!acknowledged)
    return kFlipReadbackFailed;
  return inBlank ? kFlipComplete : kFlipLate;
}

bool XenosController::collectRenderedFrame(bool drained) {
  if (_renderIndex == kNoBuffer)
    return true;

  if (drained) {
    UInt8 *memory = static_cast<UInt8 *>(_movingScratch->getBytesNoCopy());
    volatile UInt32 *fence = reinterpret_cast<volatile UInt32 *>(memory + 3 * PAGE_SIZE);
    flushMarkerCacheLine(fence);
    if (*fence == _movingSubmitted) {
      const UInt32 renderedIndex = _renderIndex;
      _renderIndex = kNoBuffer;
      if ((_movingChecked && !checkFrameNumber(_frontIndex, _movingChecked)) ||
          !checkFrameNumber(renderedIndex, _movingSubmitted)) {
        finishMovingComposite("FAIL", "scanout serial changed or render produced the wrong frame");
        return false;
      }

      const UInt32 expected = _movingChecked + _readyCount + 1;
      if (_readyCount >= kReadyCapacity || _movingSubmitted != expected) {
        ++_orderErrors;
        finishMovingComposite("FAIL", "render completed outside FIFO sequence");
        return false;
      }
      QueuedFrame &frame = _ready[_readyCount++];
      frame.number = _movingSubmitted;
      frame.index = renderedIndex;
      ++_framesQueued;
      if (_readyCount > _peakReady)
        _peakReady = _readyCount;
      if (_readyCount == kReadyCapacity)
        _queuePrimed = true;
      return true;
    }
  }

  if (++_movingWait >= kMovingWaitLimit) {
    finishMovingComposite("FAIL", drained ? "GPU completion marker missing" : "GPU ring did not drain");
    return false;
  }
  return true;
}

bool XenosController::presentQueuedFrame() {
  const QueuedFrame frame = _ready[0];
  if (frame.number != _movingChecked + 1 || frame.index == _frontIndex ||
      frame.index == _renderIndex) {
    ++_orderErrors;
    finishMovingComposite("FAIL", "ready queue head is out of order");
    return false;
  }
  const FlipResult flip = flipDuringVBlank(scanoutAddress(frame.index), _vblankHeight);
  if (flip == kFlipRetry)
    return true;
  if (flip != kFlipComplete) {
    finishMovingComposite("FAIL", flip == kFlipLate ? "flip crossed out of VBlank" : "scanout address did not latch");
    return false;
  }

  _frontIndex = frame.index;
  ++_bufferFlips[_frontIndex];
  _twoDeepFlips += _readyCount == kReadyCapacity;
  --_readyCount;
  if (_readyCount)
    _ready[0] = _ready[1];
  _vblankWaitUS = 0;

  if (!checkFrameNumber(_frontIndex, frame.number)) {
    finishMovingComposite("FAIL", "displayed frame number out of order");
    return false;
  }
  checkMovingComposite(scanoutBuffer(_frontIndex), _movingChecked);
  ++_movingChecked;
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

  if (!collectRenderedFrame(drained))
    return kFencePollUS;

  // Prime two completed frames to exercise a FIFO backlog. Thereafter a
  // ready frame may flip while the GPU is still rendering into another slot.
  const UInt32 previouslyPresented = _movingChecked;
  if (_queuePrimed && _readyCount && !presentQueuedFrame())
    return kFencePollUS;

  if (_movingChecked == kMovingFrames) {
    const bool passed = _movingSubmitted == kMovingFrames && _framesQueued == kMovingFrames &&
                        _readyCount == 0 && _peakReady == kReadyCapacity && _twoDeepFlips &&
                        _bufferFlips[0] && _bufferFlips[1] && _bufferFlips[2] &&
                        !_movingMismatches && !_orderErrors && !_lateFlips && !_latchErrors &&
                        _movingOpaque && _movingLow && _movingHigh && _movingChanges == kMovingFrames - 1;
    finishMovingComposite(passed ? "PASS" : "FAIL", "queued frame checks complete");
    return restoreDesktopScanout(drained) ? 0 : kFencePollUS;
  }

  if (_movingSubmitted < kMovingFrames && _renderIndex == kNoBuffer && drained) {
    const UInt32 index = freeScanoutIndex();
    if (index != kNoBuffer) {
      if (_ringBufferWritePtr > kXenosRingbufferLength - kMovingRingDwords) {
        // Keep each draw batch contiguous with type-2 NOPs at ring wrap.
        while (_ringBufferWritePtr)
          writeRing(2U << 30);
        syncRingBuffer();
      } else {
        _renderIndex = index;
        _movingWait = 0;
        submitMovingComposite(scanoutAddress(index));
      }
    }
  }

  if (!_queuePrimed || !_readyCount)
    return kFencePollUS;
  UInt32 delay = nextVBlankPollUS();
  if (_renderIndex != kNoBuffer && delay > kFencePollUS)
    delay = kFencePollUS;
  if (_movingChecked == previouslyPresented) {
    _vblankWaitUS += delay;
    if (_vblankWaitUS > kVBlankWaitLimitUS)
      finishMovingComposite("FAIL", "timed out waiting for a queued VBlank flip");
  }
  return delay;
}


static void prime(XenosController &test) {
    for (UInt32 frame=1; frame<=2; ++frame) {
        test.stepMovingComposite();
        assert(test._movingSubmitted == frame);
        test.completeFrame();
    }
    test.stepMovingComposite();
    assert(test._readyCount == 2 && test._movingChecked == 0);
}

int main() {
    XenosController test;
    prime(test);
    test.blank = true;
    test.stepMovingComposite();
    assert(test._movingChecked == 1 && test._frontIndex == 1 && test._renderIndex == 0);
    // Leave the third frame in flight across the second flip.
    test.stepMovingComposite();
    assert(test._movingChecked == 1);
    test.blank = false;
    test.stepMovingComposite();
    test.blank = true;
    test.stepMovingComposite();
    assert(test._movingChecked == 2 && test._frontIndex == 2 && test._renderIndex == 0);

    for (UInt32 tick=1; !test._movingFinished && tick < 20000; ++tick) {
        test.blank = tick % 17 == 0 || tick % 17 == 1;
        if (test._renderIndex != test.kNoBuffer && tick % (2 + test._movingSubmitted % 11) == 0)
            test.completeFrame();
        test.stepMovingComposite();
    }
    assert(test.passed && test._movingChecked == 480 && test._framesQueued == 480);
    assert(test._peakReady == 2 && test._twoDeepFlips > 1 && test._readyCount == 0);
    assert(test._bufferFlips[0] && test._bufferFlips[1] && test._bufferFlips[2]);

    XenosController stale;
    stale.stepMovingComposite();
    stale.completeFrame(true);
    stale.stepMovingComposite();
    assert(stale._movingFinished && stale._orderErrors == 1 && stale._movingSubmitted - stale._movingChecked == 1);

    XenosController reordered;
    prime(reordered);
    reordered._ready[0].number = 2;
    reordered.blank = true;
    reordered.stepMovingComposite();
    assert(reordered._movingFinished && reordered._orderErrors == 1 && reordered._movingChecked == 0);

    XenosController timeout;
    prime(timeout);
    for (unsigned tick=0; !timeout._movingFinished && tick<300; ++tick)
        timeout.stepMovingComposite();
    assert(timeout._movingFinished && timeout._movingChecked == 0);
    assert(timeout._movingSubmitted - timeout._movingChecked == 2);
    puts("480 ordered presentations with varying render delays passed. Queue overlapped rendering with VBlank flips and detected stale frames, FIFO reordering, and unpresented submissions.");
}
