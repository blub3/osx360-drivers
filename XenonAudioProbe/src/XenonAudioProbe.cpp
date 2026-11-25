//
//  XenonAudioProbe.cpp
//  Read-only diagnostic for the Xbox 360 audio DMA engine and its interrupt line.
//
//  XenonAudio polls the read pointer every 200 us because the audio interrupt (PCI bridge IRQ 12,
//  CPU vector 0x44) "just creates a storm": nothing public documents how the audio block
//  acknowledges it. This probe samples the audio registers and the bridge's IRQ 12 register
//  while sound plays and logs which bits change and when, to find a per-descriptor status bit.
//
//  It only reads. By default it reads audio BAR0 offsets 0x00-0x0C, the four registers XenonAudio
//  and libxenon use. With the personality property XenonAudioProbeWide it also reads 0x10-0x3C,
//  whose meaning (and whether reading them has side effects) is unknown.
//
//  With XenonAudioProbeIRQ it also routes the audio interrupt to parked thread PIR 3 for the
//  sampling time (see run()); no running CPU receives it.
//
//  Not part of the default package. Load it by hand with kextload while audio plays.
//

#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/pci/IOPCIDevice.h>

extern "C" boolean_t ml_set_interrupts_enabled(boolean_t enable);

#define kSamples            15000       // about 1.5 s at 100 us
#define kSampleDelayUS      100
#define kMaxRegs            17          // 16 audio words + bridge IRQ 12
#define kMaxChanges         160
#define kBridgeIRQ12        (0x10 + (12 * 4))

//
// Real-mode 64-bit access to the interrupt controller at 0x200_0005_0000, as in XenonSMPProbe.
//
extern "C" void xenonAudioProbeICRead(UInt32 offset, UInt64 *value);
extern "C" void xenonAudioProbeICWrite(UInt32 offset, const UInt64 *value);
extern "C" void xenonAudioProbeICEnd(void);

__asm__(
  ".text\n"
  ".align 6\n"
  ".globl _xenonAudioProbeICRead\n"
  "_xenonAudioProbeICRead:\n"
  "  mfmsr   r9\n"
  "  li      r10,0\n"
  "  ori     r10,r10,0x8010\n"   // EE | DR
  "  andc    r11,r9,r10\n"
  "  li      r12,1\n"
  "  rldimi  r11,r12,63,0\n"     // SF
  "  li      r10,0x200\n"
  "  rldicr  r10,r10,32,31\n"    // 0x200_0000_0000
  "  oris    r10,r10,0x0005\n"   // + 0x0005_0000
  "  rldicl  r3,r3,0,32\n"
  "  or      r10,r10,r3\n"
  "  mtmsrd  r11\n"
  "  isync\n"
  "  ld      r12,0(r10)\n"
  "  sync\n"
  "  mtmsrd  r9\n"
  "  isync\n"
  "  std     r12,0(r4)\n"
  "  blr\n"
  ".globl _xenonAudioProbeICWrite\n"
  "_xenonAudioProbeICWrite:\n"
  "  ld      r12,0(r4)\n"
  "  mfmsr   r9\n"
  "  li      r10,0\n"
  "  ori     r10,r10,0x8010\n"
  "  andc    r11,r9,r10\n"
  "  li      r8,1\n"
  "  rldimi  r11,r8,63,0\n"
  "  li      r10,0x200\n"
  "  rldicr  r10,r10,32,31\n"
  "  oris    r10,r10,0x0005\n"
  "  rldicl  r3,r3,0,32\n"
  "  or      r10,r10,r3\n"
  "  mtmsrd  r11\n"
  "  isync\n"
  "  std     r12,0(r10)\n"
  "  sync\n"
  "  mtmsrd  r9\n"
  "  isync\n"
  "  blr\n"
  ".globl _xenonAudioProbeICEnd\n"
  "_xenonAudioProbeICEnd:\n"
  "  blr\n"
);

static UInt64 icRead(UInt32 offset) {
  UInt64 value __attribute__((aligned(8))) = 0;
  xenonAudioProbeICRead(offset, &value);
  return value;
}

static void icWrite(UInt32 offset, UInt64 data) {
  UInt64 value __attribute__((aligned(8))) = data;
  xenonAudioProbeICWrite(offset, &value);
}

#define kICThread         3                 // parked thread, external interrupts off
#define kICBlock          (kICThread * 0x1000)
#define kICLogicalID      0x00
#define kICPriority       0x08
#define kICAck            0x50
#define kICEOI            0x60
#define kICSpurious       0x70
#define kAudioVector      0x44

struct ProbeChange {
  UInt32 sample;
  UInt32 tb;
  UInt8  reg;
  UInt32 oldValue;
  UInt32 newValue;
};

class XenonAudioProbe : public IOService {
  OSDeclareDefaultStructors(XenonAudioProbe);
  typedef IOService super;

public:
  bool start(IOService *provider);

private:
  void run(volatile UInt8 *audio, UInt32 audioWords, volatile UInt8 *bridge, bool irq);
  void runWriteTests(volatile UInt8 *audio, volatile UInt8 *bridge);
  void runEnableTests(volatile UInt8 *audio, volatile UInt8 *bridge);
  void runAckTests(volatile UInt8 *audio, volatile UInt8 *bridge);
};

OSDefineMetaClassAndStructors(XenonAudioProbe, super);

static inline UInt32 readTBL(void) {
  UInt32 tb;
  __asm__ volatile ("mftb %0" : "=r" (tb));
  return tb;
}

bool XenonAudioProbe::start(IOService *provider) {
  mach_timespec_t timeout = { 10, 0 };
  IOService       *audioDevice;
  IOService       *interruptController;
  IOPCIDevice     *audioPCI;
  IOMemoryMap     *audioMap;
  IOMemoryMap     *bridgeMap;
  UInt32          audioWords;

  if (!super::start(provider)) {
    return false;
  }

  audioDevice         = waitForService(serviceMatching("XenonAudioDevice"), &timeout);
  interruptController = waitForService(serviceMatching("XenonInterruptController"), &timeout);
  audioPCI            = (audioDevice != NULL) ? OSDynamicCast(IOPCIDevice, audioDevice->getProvider()) : NULL;
  if ((audioPCI == NULL) || (interruptController == NULL)) {
    IOLog("XenonAudioProbe: XenonAudioDevice or XenonInterruptController not found\n");
    return false;
  }

  // Extra mappings of the same registers; the drivers keep their own.
  audioMap  = audioPCI->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
  bridgeMap = interruptController->getProvider()->mapDeviceMemoryWithIndex(0);
  if ((audioMap == NULL) || (bridgeMap == NULL)) {
    IOLog("XenonAudioProbe: mapping failed\n");
    OSSafeReleaseNULL(audioMap);
    OSSafeReleaseNULL(bridgeMap);
    return false;
  }

  audioWords = (getProperty("XenonAudioProbeWide") == kOSBooleanTrue) ? 16 : 4;
  if (audioWords * 4 > audioMap->getLength()) {
    audioWords = audioMap->getLength() / 4;
  }
  IOLog("XenonAudioProbe: audio BAR0 phys 0x%lx length 0x%lx, reading %lu words; bridge phys 0x%lx\n",
        (unsigned long) audioMap->getPhysicalAddress(), (unsigned long) audioMap->getLength(),
        (unsigned long) audioWords, (unsigned long) bridgeMap->getPhysicalAddress());

  bool irq = (getProperty("XenonAudioProbeIRQ") == kOSBooleanTrue);
  UInt32 start = reinterpret_cast<UInt32>(&xenonAudioProbeICRead);
  UInt32 end   = reinterpret_cast<UInt32>(&xenonAudioProbeICEnd);
  if (irq && ((start & ~0xFFFUL) != (end & ~0xFFFUL))) {
    IOLog("XenonAudioProbe: IRQ mode skipped, IC access routine crosses a page\n");
    irq = false;
  }
  if (getProperty("XenonAudioProbeResetCh0") == kOSBooleanTrue) {
    // Repeat XenonAudioDevice::setupSampleDescriptors for channel 0, keeping Run as it was.
    volatile UInt8 *audio = (volatile UInt8 *) audioMap->getVirtualAddress();
    UInt32 desc  = OSReadLittleInt32(audio, 0x00);
    UInt32 ctl   = OSReadLittleInt32(audio, 0x08);
    UInt32 state = OSReadLittleInt32(audio, 0x04);
    OSWriteLittleInt32(audio, 0x08, 0);
    OSWriteLittleInt32(audio, 0x08, 0x2000000);
    OSWriteLittleInt32(audio, 0x00, desc);
    OSWriteLittleInt32(audio, 0x08, 0x1c08001c | (ctl & 0x01000000U));
    OSWriteLittleInt32(audio, 0x0C, 0x1c);
    IODelay(1000);
    IOLog("XenonAudioProbe: RESETCH0 before state %08lx control %08lx desc %08lx; after state %08lx control %08lx\n",
          (unsigned long) state, (unsigned long) ctl, (unsigned long) desc, (unsigned long) OSReadLittleInt32(audio, 0x04),
          (unsigned long) OSReadLittleInt32(audio, 0x08));
  } else   if ((getProperty("XenonAudioProbeStopCh1") == kOSBooleanTrue) && (audioMap->getLength() >= 0x20)) {
    // Stop channel 1 (clear Run), the state that releases the audio interrupt line.
    volatile UInt8 *audio = (volatile UInt8 *) audioMap->getVirtualAddress();
    UInt32 before = OSReadLittleInt32(audio, 0x18);
    UInt32 state  = OSReadLittleInt32(audio, 0x14);
    OSWriteLittleInt32(audio, 0x18, before & ~0x01000000U);
    IODelay(1000);
    IOLog("XenonAudioProbe: STOPCH1 ch1 control %08lx -> %08lx, state %08lx -> %08lx; bridge %08lx\n",
          (unsigned long) before, (unsigned long) OSReadLittleInt32(audio, 0x18), (unsigned long) state,
          (unsigned long) OSReadLittleInt32(audio, 0x14),
          (unsigned long) OSReadLittleInt32((volatile UInt8 *) bridgeMap->getVirtualAddress(), kBridgeIRQ12));
  } else   if (irq && (getProperty("XenonAudioProbeAck") == kOSBooleanTrue)) {
    runAckTests((volatile UInt8 *) audioMap->getVirtualAddress(), (volatile UInt8 *) bridgeMap->getVirtualAddress());
  } else   if (irq && (getProperty("XenonAudioProbeEnable") == kOSBooleanTrue)) {
    runEnableTests((volatile UInt8 *) audioMap->getVirtualAddress(), (volatile UInt8 *) bridgeMap->getVirtualAddress());
  } else   if (irq && (getProperty("XenonAudioProbeWrite") == kOSBooleanTrue) && (audioMap->getLength() >= 0x20)) {
    runWriteTests((volatile UInt8 *) audioMap->getVirtualAddress(), (volatile UInt8 *) bridgeMap->getVirtualAddress());
  } else {
    run((volatile UInt8 *) audioMap->getVirtualAddress(), audioWords, (volatile UInt8 *) bridgeMap->getVirtualAddress(), irq);
  }

  audioMap->release();
  bridgeMap->release();
  setProperty("AudioProbe", "done");
  registerService();
  return true;
}

//
// IRQ mode: route bridge IRQ 12 (audio) to parked PIR 3, which never takes external interrupts,
// and read PIR 3's acknowledge register on every sample. A pending vector 0x44 is counted and
// ended with EOI, so the log shows when the line is active and whether it stays active.
// The bridge register and PIR 3's block are restored afterwards.
//
void XenonAudioProbe::run(volatile UInt8 *audio, UInt32 audioWords, volatile UInt8 *bridge, bool irq) {
  UInt64      savedID = 0, savedPriority = 0, savedSpurious = 0;
  UInt32      savedBridge = OSReadLittleInt32(bridge, kBridgeIRQ12);
  UInt32      irqSamples = 0, irqAcks = 0, irqRuns = 0, irqLongest = 0, irqRun = 0;
  UInt32      irqOther = 0, irqOtherVector = 0, irqFirst = 0xFFFFFFFF;
  boolean_t   ints;
  UInt32      regCount = audioWords + 1;   // last one is the bridge register
  UInt32      last[kMaxRegs];
  UInt32      first[kMaxRegs];
  UInt32      changes[kMaxRegs];
  UInt16      *bitToggles;
  ProbeChange *log;
  UInt32      logged = 0;
  UInt32      tbStart, tbEnd, tb0;

  bitToggles = (UInt16 *) IOMalloc(kMaxRegs * 32 * sizeof (UInt16));
  log        = (ProbeChange *) IOMalloc(kMaxChanges * sizeof (ProbeChange));
  if ((bitToggles == NULL) || (log == NULL)) {
    IOLog("XenonAudioProbe: no memory\n");
    if (bitToggles != NULL) IOFree(bitToggles, kMaxRegs * 32 * sizeof (UInt16));
    if (log != NULL) IOFree(log, kMaxChanges * sizeof (ProbeChange));
    return;
  }
  bzero(bitToggles, kMaxRegs * 32 * sizeof (UInt16));
  bzero(changes, sizeof (changes));

  // Time base ticks per 100 ms, to convert sample times.
  tb0 = readTBL();
  IODelay(100000);
  tbStart = readTBL();
  IOLog("XenonAudioProbe: time base %lu ticks per 100 ms\n", (unsigned long) (tbStart - tb0));

  for (UInt32 r = 0; r < regCount; r++) {
    last[r] = first[r] = (r < audioWords) ? OSReadLittleInt32(audio, r * 4) : OSReadLittleInt32(bridge, kBridgeIRQ12);
  }

  if (irq) {
    ints = ml_set_interrupts_enabled(false);
    savedID       = icRead(kICBlock + kICLogicalID);
    savedPriority = icRead(kICBlock + kICPriority);
    savedSpurious = icRead(kICBlock + kICSpurious);
    icWrite(kICBlock + kICSpurious, 0x7C);
    icWrite(kICBlock + kICLogicalID, 1ULL << kICThread);
    icWrite(kICBlock + kICPriority, 0);
    for (UInt32 i = 0; (i < 64) && ((icRead(kICBlock + kICAck) & 0x7F) != 0x7C); i++) {
      icWrite(kICBlock + kICEOI, 0);
    }
    ml_set_interrupts_enabled(ints);
    // Enabled, target PIR 3, like XenonInterruptController::enableVector for CPU 0.
    OSWriteLittleInt32(bridge, kBridgeIRQ12, 0x00800000 | ((1U << kICThread) << 8) | 0x80 | (kAudioVector >> 2));
    IOLog("XenonAudioProbe: IRQ mode, bridge IRQ 12 %08lx -> %08lx, PIR 3 block was ID %lx priority %lx spurious %lx\n",
          (unsigned long) savedBridge, (unsigned long) OSReadLittleInt32(bridge, kBridgeIRQ12),
          (unsigned long) savedID, (unsigned long) savedPriority, (unsigned long) savedSpurious);
  }

  tbStart = readTBL();
  for (UInt32 s = 0; s < kSamples; s++) {
    UInt32 tb = readTBL();
    bool   pending = false;
    if (irq) {
      ints = ml_set_interrupts_enabled(false);
      UInt32 v = icRead(kICBlock + kICAck) & 0x7F;
      if (v != 0x7C) {
        icWrite(kICBlock + kICEOI, 0);
        irqAcks++;
        pending = (v == kAudioVector);
        if (!pending) {
          irqOther++;
          irqOtherVector = v;
        }
      }
      ml_set_interrupts_enabled(ints);
      if (pending) {
        if (irqFirst == 0xFFFFFFFF) irqFirst = s;
        irqSamples++;
        irqRun++;
        if (irqRun == 1) irqRuns++;
        if (irqRun > irqLongest) irqLongest = irqRun;
      } else {
        irqRun = 0;
      }
    }
    for (UInt32 r = 0; r < regCount; r++) {
      UInt32 v = (r < audioWords) ? OSReadLittleInt32(audio, r * 4) : OSReadLittleInt32(bridge, kBridgeIRQ12);
      UInt32 diff = v ^ last[r];
      if (diff != 0) {
        changes[r]++;
        for (UInt32 b = 0; b < 32; b++) {
          if ((diff >> b) & 1) {
            bitToggles[(r * 32) + b]++;
          }
        }
        bool routine = (r == 1) && ((diff & ~0xFFFF001FU) == 0);
        if (!routine && (logged < kMaxChanges)) {
          log[logged].sample   = s;
          log[logged].tb       = tb - tbStart;
          log[logged].reg      = r;
          log[logged].oldValue = last[r];
          log[logged].newValue = v;
          logged++;
        }
        last[r] = v;
      }
    }
    if (pending && (irqRun == 1) && (logged < kMaxChanges)) {
      // Start of a pending run: record it with the current state and control values.
      log[logged].sample   = s;
      log[logged].tb       = tb - tbStart;
      log[logged].reg      = 0xFF;
      log[logged].oldValue = last[1];
      log[logged].newValue = (audioWords > 2) ? last[2] : 0;
      logged++;
    }
    IODelay(kSampleDelayUS);
  }
  tbEnd = readTBL();

  if (irq) {
    OSWriteLittleInt32(bridge, kBridgeIRQ12, savedBridge);
    ints = ml_set_interrupts_enabled(false);
    for (UInt32 i = 0; (i < 64) && ((icRead(kICBlock + kICAck) & 0x7F) != 0x7C); i++) {
      icWrite(kICBlock + kICEOI, 0);
    }
    icWrite(kICBlock + kICSpurious, savedSpurious);
    icWrite(kICBlock + kICPriority, savedPriority);
    icWrite(kICBlock + kICLogicalID, savedID);
    ml_set_interrupts_enabled(ints);
  }
  IOLog("XenonAudioProbe: %lu samples over %lu ticks\n", (unsigned long) kSamples, (unsigned long) (tbEnd - tbStart));
  for (UInt32 r = 0; r < regCount; r++) {
    char name[16];
    if (r < audioWords) {
      snprintf(name, sizeof (name), "audio+%02lx", (unsigned long) (r * 4));
    } else {
      snprintf(name, sizeof (name), "bridge+%02x", kBridgeIRQ12);
    }
    IOLog("XenonAudioProbe: %s first %08lx last %08lx changes %lu\n", name, (unsigned long) first[r],
          (unsigned long) last[r], (unsigned long) changes[r]);
    for (UInt32 b = 0; b < 32; b++) {
      if (bitToggles[(r * 32) + b] != 0) {
        IOLog("XenonAudioProbe: %s bit %lu toggled %u times\n", name, (unsigned long) b, bitToggles[(r * 32) + b]);
      }
    }
    IOSleep(20);
  }

  for (UInt32 i = 0; i < logged; i++) {
    if (log[i].reg == 0xFF) {
      IOLog("XenonAudioProbe: #%lu s%lu t%lu IRQ 0x44 pending, state %08lx control %08lx\n", (unsigned long) i,
            (unsigned long) log[i].sample, (unsigned long) log[i].tb, (unsigned long) log[i].oldValue,
            (unsigned long) log[i].newValue);
      continue;
    }
    const char *name = (log[i].reg < audioWords) ? "audio" : "bridge";
    UInt32 offset = (log[i].reg < audioWords) ? (log[i].reg * 4) : kBridgeIRQ12;
    IOLog("XenonAudioProbe: #%lu s%lu t%lu %s+%02lx %08lx -> %08lx\n", (unsigned long) i, (unsigned long) log[i].sample,
          (unsigned long) log[i].tb, name, (unsigned long) offset, (unsigned long) log[i].oldValue,
          (unsigned long) log[i].newValue);
    if ((i % 4) == 3) {
      IOSleep(50);   // keep the kernel log from dropping lines
    }
  }
  // Summary last, twice, so syslogd keeps at least one copy.
  for (UInt32 copy = 0; copy < 2; copy++) {
    IOSleep(300);
    IOLog("XenonAudioProbe: SUMMARY %lu samples over %lu ticks; state %lu changes, control %lu changes\n",
          (unsigned long) kSamples, (unsigned long) (tbEnd - tbStart), (unsigned long) changes[1],
          (unsigned long) ((audioWords > 2) ? changes[2] : 0));
    for (UInt32 r = 0; r < regCount; r++) {
      UInt32 mask = 0;
      for (UInt32 b = 0; b < 32; b++) {
        if (bitToggles[(r * 32) + b] != 0) mask |= (1U << b);
      }
      IOLog("XenonAudioProbe: SUMMARY %s+%02lx first %08lx last %08lx changes %lu toggling bits %08lx\n",
            (r < audioWords) ? "audio" : "bridge", (unsigned long) ((r < audioWords) ? (r * 4) : kBridgeIRQ12),
            (unsigned long) first[r], (unsigned long) last[r], (unsigned long) changes[r], (unsigned long) mask);
      if ((r % 4) == 3) IOSleep(50);
    }
    if (irq) {
      IOLog("XenonAudioProbe: SUMMARY IRQ 0x44 pending in %lu samples, %lu runs, longest %lu, first at sample %ld; "
            "%lu acks, %lu other vectors (last %02lx); bridge restored to %08lx\n",
            (unsigned long) irqSamples, (unsigned long) irqRuns, (unsigned long) irqLongest,
            (irqFirst == 0xFFFFFFFF) ? -1L : (long) irqFirst, (unsigned long) irqAcks, (unsigned long) irqOther,
            (unsigned long) irqOtherVector, (unsigned long) OSReadLittleInt32(bridge, kBridgeIRQ12));
    }
  }
  IOLog("XenonAudioProbe: done, %lu changes logged\n", (unsigned long) logged);

  IOFree(bitToggles, kMaxRegs * 32 * sizeof (UInt16));
  IOFree(log, kMaxChanges * sizeof (ProbeChange));
}

//
// Write tests (XenonAudioProbeIRQ + XenonAudioProbeWrite), meant to run while no audio plays.
// The audio interrupt is routed to parked PIR 3 as in IRQ mode. Each step writes one audio
// register, counts for 200 samples how often vector 0x44 is pending and how often the bridge
// reports the line (bit 24), then restores the register. Channel 0 is 0x00-0x0C (the one
// XenonAudio uses); channel 1 is 0x10-0x1C.
//
#define kWriteSamples   200
#define kRunBit         0x01000000U

struct WriteStep {
  const char *what;
  UInt32      offset;
  UInt32      orMask;
  UInt32      andMask;
};

void XenonAudioProbe::runWriteTests(volatile UInt8 *audio, volatile UInt8 *bridge) {
  static const WriteStep steps[] = {
    { "baseline (no write)",          0xFF, 0,          0xFFFFFFFF },
    { "ch1 control: clear Run",       0x18, 0,          ~kRunBit },
    { "ch1 control: write 0",         0x18, 0,          0 },
    { "ch1 control: clear 0x1C",      0x18, 0,          ~0x1CU },
    { "ch1 control: set 0x03",        0x18, 0x03,       0xFFFFFFFF },
    { "ch1 state: write 0",           0x14, 0,          0 },
    { "ch0 control: set 0x03",        0x08, 0x03,       0xFFFFFFFF },
    { "ch0 control: clear 0x03",      0x08, 0,          ~0x03U },
    { "ch0 control: set 0x1C",        0x08, 0x1C,       0xFFFFFFFF },
    { "ch0 control: write 0",         0x08, 0,          0 },
    { "both controls: write 0",       0x88, 0,          0 },
  };
  UInt32    savedBridge = OSReadLittleInt32(bridge, kBridgeIRQ12);
  UInt32    saved[8];
  UInt64    savedID, savedPriority, savedSpurious;
  boolean_t ints;

  for (UInt32 r = 0; r < 8; r++) {
    saved[r] = OSReadLittleInt32(audio, r * 4);
  }
  IOLog("XenonAudioProbe: WRITE before: ch0 %08lx %08lx %08lx %08lx ch1 %08lx %08lx %08lx %08lx bridge %08lx\n",
        (unsigned long) saved[0], (unsigned long) saved[1], (unsigned long) saved[2], (unsigned long) saved[3],
        (unsigned long) saved[4], (unsigned long) saved[5], (unsigned long) saved[6], (unsigned long) saved[7],
        (unsigned long) savedBridge);
  if ((saved[2] & kRunBit) != 0) {
    IOLog("XenonAudioProbe: WRITE skipped, channel 0 is running (stop audio first)\n");
    return;
  }

  ints = ml_set_interrupts_enabled(false);
  savedID       = icRead(kICBlock + kICLogicalID);
  savedPriority = icRead(kICBlock + kICPriority);
  savedSpurious = icRead(kICBlock + kICSpurious);
  icWrite(kICBlock + kICSpurious, 0x7C);
  icWrite(kICBlock + kICLogicalID, 1ULL << kICThread);
  icWrite(kICBlock + kICPriority, 0);
  for (UInt32 i = 0; (i < 64) && ((icRead(kICBlock + kICAck) & 0x7F) != 0x7C); i++) {
    icWrite(kICBlock + kICEOI, 0);
  }
  ml_set_interrupts_enabled(ints);
  OSWriteLittleInt32(bridge, kBridgeIRQ12, 0x00800000 | ((1U << kICThread) << 8) | 0x80 | (kAudioVector >> 2));

  for (UInt32 n = 0; n < (sizeof (steps) / sizeof (steps[0])); n++) {
    const WriteStep *st = &steps[n];
    UInt32 written0 = 0, written1 = 0;

    if (st->offset == 0x88) {
      OSWriteLittleInt32(audio, 0x08, 0);
      OSWriteLittleInt32(audio, 0x18, 0);
    } else if (st->offset != 0xFF) {
      written0 = (OSReadLittleInt32(audio, st->offset) | st->orMask) & st->andMask;
      OSWriteLittleInt32(audio, st->offset, written0);
    }
    IODelay(200);

    UInt32 pending = 0, line = 0;
    for (UInt32 i = 0; i < kWriteSamples; i++) {
      ints = ml_set_interrupts_enabled(false);
      UInt32 v = icRead(kICBlock + kICAck) & 0x7F;
      if (v != 0x7C) {
        icWrite(kICBlock + kICEOI, 0);
      }
      ml_set_interrupts_enabled(ints);
      pending += (v == kAudioVector) ? 1 : 0;
      line    += ((OSReadLittleInt32(bridge, kBridgeIRQ12) & 0x01000000) != 0) ? 1 : 0;
      IODelay(kSampleDelayUS);
    }
    written1 = (st->offset < 0x20) ? OSReadLittleInt32(audio, st->offset) : 0;

    IOLog("XenonAudioProbe: WRITE %-26s wrote %08lx read back %08lx: pending %lu/%u, line %lu/%u; ch0 %08lx %08lx ch1 %08lx %08lx\n",
          st->what, (unsigned long) written0, (unsigned long) written1, (unsigned long) pending, kWriteSamples,
          (unsigned long) line, kWriteSamples, (unsigned long) OSReadLittleInt32(audio, 0x04),
          (unsigned long) OSReadLittleInt32(audio, 0x08), (unsigned long) OSReadLittleInt32(audio, 0x14),
          (unsigned long) OSReadLittleInt32(audio, 0x18));

    // Restore everything this step may have changed, in register order (address, state, control).
    for (UInt32 ch = 0; ch < 2; ch++) {
      OSWriteLittleInt32(audio, (ch * 0x10) + 0x04, saved[(ch * 4) + 1]);
      OSWriteLittleInt32(audio, (ch * 0x10) + 0x08, saved[(ch * 4) + 2]);
    }
    IODelay(1000);
    IOSleep(30);
  }

  OSWriteLittleInt32(bridge, kBridgeIRQ12, savedBridge);
  ints = ml_set_interrupts_enabled(false);
  for (UInt32 i = 0; (i < 64) && ((icRead(kICBlock + kICAck) & 0x7F) != 0x7C); i++) {
    icWrite(kICBlock + kICEOI, 0);
  }
  icWrite(kICBlock + kICSpurious, savedSpurious);
  icWrite(kICBlock + kICPriority, savedPriority);
  icWrite(kICBlock + kICLogicalID, savedID);
  ml_set_interrupts_enabled(ints);

  IOSleep(200);
  IOLog("XenonAudioProbe: WRITE after: ch0 %08lx %08lx %08lx %08lx ch1 %08lx %08lx %08lx %08lx bridge %08lx (restored)\n",
        (unsigned long) OSReadLittleInt32(audio, 0x00), (unsigned long) OSReadLittleInt32(audio, 0x04),
        (unsigned long) OSReadLittleInt32(audio, 0x08), (unsigned long) OSReadLittleInt32(audio, 0x0C),
        (unsigned long) OSReadLittleInt32(audio, 0x10), (unsigned long) OSReadLittleInt32(audio, 0x14),
        (unsigned long) OSReadLittleInt32(audio, 0x18), (unsigned long) OSReadLittleInt32(audio, 0x1C),
        (unsigned long) OSReadLittleInt32(bridge, kBridgeIRQ12));
}

//
// Enable tests (XenonAudioProbeIRQ + XenonAudioProbeEnable), meant to run while channel 0 plays.
// Channel 0's control register takes interrupt enables in bits 2-4 (0x1C at init, not readable).
// For each enable set, rewrite the control register with Run kept, then sample for about 120 ms
// (about 11 descriptors) and record pending runs against the read pointer and state flags.
// 0x1C is written back at the end. Interrupts go to parked PIR 3, as in IRQ mode.
//
#define kEnableSamples  1000

void XenonAudioProbe::runEnableTests(volatile UInt8 *audio, volatile UInt8 *bridge) {
  static const UInt32 enables[] = { 0x00, 0x04, 0x08, 0x10, 0x1C };
  UInt32    savedBridge = OSReadLittleInt32(bridge, kBridgeIRQ12);
  UInt32    control = OSReadLittleInt32(audio, 0x08);
  UInt64    savedID, savedPriority, savedSpurious;
  boolean_t ints;

  IOLog("XenonAudioProbe: ENABLE before: ch0 state %08lx control %08lx ch1 control %08lx\n",
        (unsigned long) OSReadLittleInt32(audio, 0x04), (unsigned long) control,
        (unsigned long) OSReadLittleInt32(audio, 0x18));
  if ((control & kRunBit) == 0) {
    IOLog("XenonAudioProbe: ENABLE skipped, channel 0 is not running (play audio first)\n");
    return;
  }

  ints = ml_set_interrupts_enabled(false);
  savedID       = icRead(kICBlock + kICLogicalID);
  savedPriority = icRead(kICBlock + kICPriority);
  savedSpurious = icRead(kICBlock + kICSpurious);
  icWrite(kICBlock + kICSpurious, 0x7C);
  icWrite(kICBlock + kICLogicalID, 1ULL << kICThread);
  icWrite(kICBlock + kICPriority, 0);
  for (UInt32 i = 0; (i < 64) && ((icRead(kICBlock + kICAck) & 0x7F) != 0x7C); i++) {
    icWrite(kICBlock + kICEOI, 0);
  }
  ml_set_interrupts_enabled(ints);
  OSWriteLittleInt32(bridge, kBridgeIRQ12, 0x00800000 | ((1U << kICThread) << 8) | 0x80 | (kAudioVector >> 2));

  for (UInt32 n = 0; n < (sizeof (enables) / sizeof (enables[0])); n++) {
    UInt32 base = OSReadLittleInt32(audio, 0x08) & 0xFFFFFF00U;
    UInt32 pending = 0, runs = 0, run = 0, longest = 0, line = 0;
    UInt32 starvedSamples = 0, starvedPending = 0, readPtrChanges = 0;
    UInt32 lastReadPtr = OSReadLittleInt32(audio, 0x04) & 0x1F;
    UInt32 firstRunState[4] = { 0, 0, 0, 0 };
    UInt32 firstRunCtl[4]   = { 0, 0, 0, 0 };
    UInt32 firstRunSample[4] = { 0, 0, 0, 0 };

    OSWriteLittleInt32(audio, 0x08, base | enables[n]);
    IODelay(200);
    // Drop what was latched under the previous setting.
    ints = ml_set_interrupts_enabled(false);
    for (UInt32 i = 0; (i < 64) && ((icRead(kICBlock + kICAck) & 0x7F) != 0x7C); i++) {
      icWrite(kICBlock + kICEOI, 0);
    }
    ml_set_interrupts_enabled(ints);

    for (UInt32 i = 0; i < kEnableSamples; i++) {
      ints = ml_set_interrupts_enabled(false);
      UInt32 v = icRead(kICBlock + kICAck) & 0x7F;
      if (v != 0x7C) {
        icWrite(kICBlock + kICEOI, 0);
      }
      ml_set_interrupts_enabled(ints);
      UInt32 state = OSReadLittleInt32(audio, 0x04);
      UInt32 ctl   = OSReadLittleInt32(audio, 0x08);
      bool   p     = (v == kAudioVector);
      bool   starved = (ctl & 0x02) == 0;
      if ((state & 0x1F) != lastReadPtr) {
        readPtrChanges++;
        lastReadPtr = state & 0x1F;
      }
      starvedSamples += starved ? 1 : 0;
      starvedPending += (starved && p) ? 1 : 0;
      line += ((OSReadLittleInt32(bridge, kBridgeIRQ12) & 0x01000000) != 0) ? 1 : 0;
      if (p) {
        pending++;
        run++;
        if (run == 1) {
          if (runs < 4) {
            firstRunState[runs]  = state;
            firstRunCtl[runs]    = ctl;
            firstRunSample[runs] = i;
          }
          runs++;
        }
        if (run > longest) longest = run;
      } else {
        run = 0;
      }
      IODelay(kSampleDelayUS);
    }

    IOLog("XenonAudioProbe: ENABLE %02lx: pending %lu/%u in %lu runs (longest %lu), line %lu, read pointer moved %lu times, "
          "starved %lu samples (%lu pending)\n", (unsigned long) enables[n], (unsigned long) pending, kEnableSamples,
          (unsigned long) runs, (unsigned long) longest, (unsigned long) line, (unsigned long) readPtrChanges,
          (unsigned long) starvedSamples, (unsigned long) starvedPending);
    for (UInt32 r = 0; (r < runs) && (r < 4); r++) {
      IOLog("XenonAudioProbe: ENABLE %02lx run %lu at sample %lu: state %08lx control %08lx\n", (unsigned long) enables[n],
            (unsigned long) r, (unsigned long) firstRunSample[r], (unsigned long) firstRunState[r], (unsigned long) firstRunCtl[r]);
    }
    IOSleep(40);
  }

  // Back to the init value's enables, then restore routing and PIR 3's block.
  OSWriteLittleInt32(audio, 0x08, (OSReadLittleInt32(audio, 0x08) & 0xFFFFFF00U) | 0x1C);
  OSWriteLittleInt32(bridge, kBridgeIRQ12, savedBridge);
  ints = ml_set_interrupts_enabled(false);
  for (UInt32 i = 0; (i < 64) && ((icRead(kICBlock + kICAck) & 0x7F) != 0x7C); i++) {
    icWrite(kICBlock + kICEOI, 0);
  }
  icWrite(kICBlock + kICSpurious, savedSpurious);
  icWrite(kICBlock + kICPriority, savedPriority);
  icWrite(kICBlock + kICLogicalID, savedID);
  ml_set_interrupts_enabled(ints);
  IOSleep(200);
  IOLog("XenonAudioProbe: ENABLE after: ch0 state %08lx control %08lx, bridge %08lx (restored)\n",
        (unsigned long) OSReadLittleInt32(audio, 0x04), (unsigned long) OSReadLittleInt32(audio, 0x08),
        (unsigned long) OSReadLittleInt32(bridge, kBridgeIRQ12));
}

//
// Ack tests (XenonAudioProbeIRQ + XenonAudioProbeAck), meant to run while channel 0 plays.
// Whenever vector 0x44 is pending, rewrite channel 0's control register with the candidate
// acknowledge bits (Run kept), then record when the line comes back. Interrupts go to parked
// PIR 3. Control is left with enables 0x1C, as XenonAudio programs it.
//
#define kAckSamples     2000

void XenonAudioProbe::runAckTests(volatile UInt8 *audio, volatile UInt8 *bridge) {
  static const UInt32 acks[] = { 0x08, 0x1C, 0x02 };
  UInt32    savedBridge = OSReadLittleInt32(bridge, kBridgeIRQ12);
  UInt64    savedID, savedPriority, savedSpurious;
  boolean_t ints;

  if ((OSReadLittleInt32(audio, 0x08) & kRunBit) == 0) {
    IOLog("XenonAudioProbe: ACK skipped, channel 0 is not running (play audio first)\n");
    return;
  }

  ints = ml_set_interrupts_enabled(false);
  savedID       = icRead(kICBlock + kICLogicalID);
  savedPriority = icRead(kICBlock + kICPriority);
  savedSpurious = icRead(kICBlock + kICSpurious);
  icWrite(kICBlock + kICSpurious, 0x7C);
  icWrite(kICBlock + kICLogicalID, 1ULL << kICThread);
  icWrite(kICBlock + kICPriority, 0);
  for (UInt32 i = 0; (i < 64) && ((icRead(kICBlock + kICAck) & 0x7F) != 0x7C); i++) {
    icWrite(kICBlock + kICEOI, 0);
  }
  ml_set_interrupts_enabled(ints);
  OSWriteLittleInt32(bridge, kBridgeIRQ12, 0x00800000 | ((1U << kICThread) << 8) | 0x80 | (kAudioVector >> 2));

  for (UInt32 n = 0; n < (sizeof (acks) / sizeof (acks[0])); n++) {
    UInt32 asserts = 0, pendingSamples = 0, lastAssert = 0, minGap = 0xFFFFFFFF, maxGap = 0, sumGap = 0, gaps = 0;
    UInt32 firstState[12], firstSample[12], firstLen[12];
    UInt32 tb0 = readTBL(), tb1;

    for (UInt32 i = 0; i < kAckSamples; i++) {
      ints = ml_set_interrupts_enabled(false);
      UInt32 v = icRead(kICBlock + kICAck) & 0x7F;
      if (v != 0x7C) {
        icWrite(kICBlock + kICEOI, 0);
      }
      ml_set_interrupts_enabled(ints);
      if (v == kAudioVector) {
        UInt32 state = OSReadLittleInt32(audio, 0x04);
        // Count a new assertion only if the previous sample was clear.
        if ((i == 0) || (lastAssert != i - 1)) {
          if (asserts < 12) {
            firstState[asserts]  = state;
            firstSample[asserts] = i;
            firstLen[asserts]    = 0;
          }
          asserts++;
        } else if (asserts <= 12) {
          firstLen[asserts - 1]++;
        }
        if (lastAssert != 0) {
          UInt32 gap = i - lastAssert;
          if (gap > 1) {
            if (gap < minGap) minGap = gap;
            if (gap > maxGap) maxGap = gap;
            sumGap += gap;
            gaps++;
          }
        }
        lastAssert = i;
        pendingSamples++;
        // Candidate acknowledge: rewrite control with Run kept.
        OSWriteLittleInt32(audio, 0x08, (OSReadLittleInt32(audio, 0x08) & 0xFFFFFF00U) | acks[n]);
      }
      IODelay(kSampleDelayUS);
    }
    tb1 = readTBL();

    IOLog("XenonAudioProbe: ACK %02lx: %lu assertions, pending %lu/%u samples, gap between assertions min %lu max %lu mean %lu samples; "
          "%lu ticks for %u samples\n", (unsigned long) acks[n], (unsigned long) asserts, (unsigned long) pendingSamples,
          kAckSamples, (unsigned long) ((gaps > 0) ? minGap : 0), (unsigned long) maxGap,
          (unsigned long) ((gaps > 0) ? (sumGap / gaps) : 0), (unsigned long) (tb1 - tb0), kAckSamples);
    for (UInt32 k = 0; (k < asserts) && (k < 12); k++) {
      IOLog("XenonAudioProbe: ACK %02lx #%lu sample %lu (+%lu more): state %08lx\n", (unsigned long) acks[n], (unsigned long) k,
            (unsigned long) firstSample[k], (unsigned long) firstLen[k], (unsigned long) firstState[k]);
      if ((k % 4) == 3) IOSleep(40);
    }
    // Back to the enables XenonAudio programs.
    OSWriteLittleInt32(audio, 0x08, (OSReadLittleInt32(audio, 0x08) & 0xFFFFFF00U) | 0x1C);
    IOSleep(60);
  }

  OSWriteLittleInt32(bridge, kBridgeIRQ12, savedBridge);
  ints = ml_set_interrupts_enabled(false);
  for (UInt32 i = 0; (i < 64) && ((icRead(kICBlock + kICAck) & 0x7F) != 0x7C); i++) {
    icWrite(kICBlock + kICEOI, 0);
  }
  icWrite(kICBlock + kICSpurious, savedSpurious);
  icWrite(kICBlock + kICPriority, savedPriority);
  icWrite(kICBlock + kICLogicalID, savedID);
  ml_set_interrupts_enabled(ints);
  IOSleep(200);
  IOLog("XenonAudioProbe: ACK after: ch0 state %08lx control %08lx, bridge %08lx (restored)\n",
        (unsigned long) OSReadLittleInt32(audio, 0x04), (unsigned long) OSReadLittleInt32(audio, 0x08),
        (unsigned long) OSReadLittleInt32(bridge, kBridgeIRQ12));
}
