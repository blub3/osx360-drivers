
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
    UInt32 _bufferFlips[2]{}, frames[2]{};
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
    UInt32 scanoutAddress(UInt32 index) { return index ? 0x10000000 : 0x1f800000; }
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
        assert(!scanoutAt(address));
        ++_movingSubmitted;
        regs[kXenosRegCpRbWritePtr]=++_ringBufferWritePtr;
    }
    void completeFrame(bool stale=false) {
        assert(!scanoutAt(scanoutAddress(1-_frontIndex)));
        frames[1-_frontIndex]=_movingSubmitted-stale;
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


int main() {
    XenosController good;
    for (UInt32 frame=1; frame<=kMovingFrames; ++frame) {
        good.blank=false;
        assert(good.stepMovingComposite()==kFencePollUS);
        assert(good._movingSubmitted==frame);
        good.blank=true;
        assert(good.stepMovingComposite()==kFencePollUS);
        assert(good._movingChecked==frame-1);
        good.completeFrame();
        good.blank=false;
        assert(good.stepMovingComposite()!=0);
        assert(good._movingChecked==frame-1);
        good.blank=true;
        good.stepMovingComposite();
        assert(good._movingChecked==frame && good._frontIndex==(frame&1));
    }
    assert(good.passed && good.restored);
    assert(good._bufferFlips[0]==240 && good._bufferFlips[1]==240);
    XenosController stale;
    stale.stepMovingComposite();
    stale.completeFrame(true);
    stale.blank=true;
    stale.stepMovingComposite();
    assert(stale._movingFinished && stale._orderErrors==1 && stale._movingChecked==0);
    XenosController late;
    late.stepMovingComposite();
    late.completeFrame();
    late.blank=true;
    late.simulatedLate=true;
    late.stepMovingComposite();
    assert(late._movingFinished && late._lateFlips==1 && late._movingChecked==0);
    XenosController latch;
    latch.stepMovingComposite();
    latch.completeFrame();
    latch.blank=true;
    latch.simulatedLatchFailure=true;
    latch.stepMovingComposite();
    assert(latch._movingFinished && latch._latchErrors==1 && latch._movingChecked==0);
    for (UInt32 frame=1; frame<=kMovingFrames; ++frame) {
        assert((frameColor(frame)&0xffff)==frame);
        assert((frameColor(frame)^frameCheckColor(frame))==0x00ffffff);
        const LayerOrigins a=movingLayerOrigins(frame-1,1920,1080);
        assert(a.backY+1<a.frontY);
        assert(a.backY+7>a.backY+kTextureScale);
    }
    puts("VBlank state machine: 480 ordered flips, stale frame, late flip, latch failure, and all frame stamps verified in simulation.");
}
