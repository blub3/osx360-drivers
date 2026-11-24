//
//  XenonSMPProbe.cpp
//  Diagnostic for the secondary hardware threads XeLL parks in OpenBIOS.
//
//  XeLL enters a loaded image with thread 0 at offset 0x0 and the other threads at offset 0x60.
//  OpenBIOS (linked at 0x07E00000, outside XNU's memory) parks them there in "b .".
//
//  Default mode only reads the parking area. With the XenonSMPProbeRelease personality
//  property, it briefly sends the parked threads through a counting loop in the same area,
//  then parks them again. Nothing outside 0x07E00060-0x07E000DF is written.
//

#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>

extern "C" unsigned int ml_phys_read(vm_offset_t paddr);
extern "C" void ml_phys_write(vm_offset_t paddr, unsigned int data);
extern "C" void invalidate_icache(vm_offset_t addr, unsigned cnt, int phys);
extern "C" boolean_t ml_set_interrupts_enabled(boolean_t enable);

#define kParkBase       0x07E00000
#define kParkEntry      (kParkBase + 0x60)
#define kProbeCode      (kParkBase + 0x64)
#define kCounterBase    (kParkBase + 0xC0)
#define kThreadCount    6

#define kPPCBranchSelf  0x48000000  // b .
#define kPPCBranchNext  0x48000004  // b .+4

// Counting loop for 0x64-0x80. Each thread increments the word at 0xC0 + PIR * 4, then
// returns to 0x60. Rewriting 0x60 back to "b ." parks the threads again.
static const UInt32 kProbeLoop[] = {
  0x7C7FFAA6, // mfspr  r3, PIR
  0x5463103A, // slwi   r3, r3, 2
  0x3C8007E0, // lis    r4, 0x07E0
  0x608400C0, // ori    r4, r4, 0xC0
  0x7CA4182E, // lwzx   r5, r4, r3
  0x38A50001, // addi   r5, r5, 1
  0x7CA4192E, // stwx   r5, r4, r3
  0x4BFFFFE0  // b      0x60
};

class XenonSMPProbe : public IOService {
  OSDeclareDefaultStructors(XenonSMPProbe);
  typedef IOService super;

public:
  bool start(IOService *provider);

private:
  void publishWords(const char *key, UInt32 phys, UInt32 count);
  void writeCode(UInt32 phys, UInt32 word);
  void runReleaseProbe(void);
  void runSPRProbe(void);
};

//
// SPR probe. Each parked thread records its state at 0x07E00C00 + PIR * 0x40, which lies in
// OpenBIOS's old 0xC00 vector, unused once XNU runs. The area is restored afterwards.
// Record: MSR, HID0, HID1, HID4, HID6, LPCR, HRMOR, TB (64-bit each).
//
#define kRecordBase     (kParkBase + 0xC00)
#define kRecordSize     0x40
#define kSPRCodeWords   21  // index of the final branch; the code is 22 words

static inline UInt32 encodeMfspr(UInt32 rt, UInt32 spr) {
  return 0x7C0002A6 | (rt << 21) | ((spr & 0x1F) << 16) | (((spr >> 5) & 0x1F) << 11);
}

static inline UInt32 encodeStd(UInt32 rs, UInt32 ra, UInt32 ds) {
  return 0xF8000000 | (rs << 21) | (ra << 16) | (ds & 0xFFFC);
}

static void buildSPRCode(UInt32 *code) {
  static const UInt32 sprs[] = { 1008, 1009, 1012, 1017, 318, 313 };  // HID0, HID1, HID4, HID6, LPCR, HRMOR
  UInt32 n = 0;

  code[n++] = encodeMfspr(3, 1023);         // mfspr r3, PIR
  code[n++] = 0x54633032;                   // slwi  r3, r3, 6
  code[n++] = 0x3C8007E0;                   // lis   r4, 0x07E0
  code[n++] = 0x60840C00;                   // ori   r4, r4, 0x0C00
  code[n++] = 0x7C841A14;                   // add   r4, r4, r3
  code[n++] = 0x7CA000A6;                   // mfmsr r5
  code[n++] = encodeStd(5, 4, 0);
  for (UInt32 i = 0; i < 6; i++) {
    code[n++] = encodeMfspr(5, sprs[i]);
    code[n++] = encodeStd(5, 4, 8 * (i + 1));
  }
  code[n++] = 0x7CAC42E6;                   // mftb  r5
  code[n++] = encodeStd(5, 4, 56);
  code[n] = 0x48000000 | ((0x60 - (0x64 + n * 4)) & 0x03FFFFFC);  // b 0x60
}

// The running thread's own values, in the same layout.
static void readOwnSPRs(UInt64 *out) {
  __asm__ volatile (
    "mfmsr r5\n  std r5,0(%0)\n"
    "mfspr r5,1008\n  std r5,8(%0)\n"
    "mfspr r5,1009\n  std r5,16(%0)\n"
    "mfspr r5,1012\n  std r5,24(%0)\n"
    "mfspr r5,1017\n  std r5,32(%0)\n"
    "mfspr r5,318\n  std r5,40(%0)\n"
    "mfspr r5,313\n  std r5,48(%0)\n"
    "mftb r5\n  std r5,56(%0)\n"
    : : "b" (out) : "r5", "memory");
}

static void logRecord(const char *who, const UInt32 *w) {
  IOLog("XenonSMPProbe: %s MSR %08lx%08lx HID0 %08lx%08lx HID1 %08lx%08lx HID4 %08lx%08lx\n", who,
        (unsigned long) w[0], (unsigned long) w[1], (unsigned long) w[2], (unsigned long) w[3],
        (unsigned long) w[4], (unsigned long) w[5], (unsigned long) w[6], (unsigned long) w[7]);
  IOLog("XenonSMPProbe: %s HID6 %08lx%08lx LPCR %08lx%08lx HRMOR %08lx%08lx TB %08lx%08lx\n", who,
        (unsigned long) w[8], (unsigned long) w[9], (unsigned long) w[10], (unsigned long) w[11],
        (unsigned long) w[12], (unsigned long) w[13], (unsigned long) w[14], (unsigned long) w[15]);
}

OSDefineMetaClassAndStructors(XenonSMPProbe, IOService);

void XenonSMPProbe::publishWords(const char *key, UInt32 phys, UInt32 count) {
  UInt32 words[16];

  for (UInt32 i = 0; (i < count) && (i < 16); i++) {
    words[i] = ml_phys_read(phys + (i * 4));
  }
  setProperty(key, words, count * sizeof (UInt32));

  // ioreg on this system stops at the IODTNVRAM entry, so also log the values.
  IOLog("XenonSMPProbe: %s @0x%08lx:", key, (unsigned long) phys);
  for (UInt32 i = 0; (i < count) && (i < 16); i++) {
    IOLog(" %08lx", (unsigned long) words[i]);
  }
  IOLog("\n");
}

void XenonSMPProbe::writeCode(UInt32 phys, UInt32 word) {
  ml_phys_write(phys, word);
  invalidate_icache(phys, sizeof (word), 1);
}

void XenonSMPProbe::runReleaseProbe(void) {
  boolean_t ints;

  // Only proceed if the parking area holds exactly what OpenBIOS left there.
  if ((ml_phys_read(kParkEntry) != kPPCBranchSelf) || ((ml_phys_read(kParkBase) & 0xFC000003) != 0x48000000)) {
    setProperty("ReleaseProbe", "skipped: parking loop not found");
    IOLog("XenonSMPProbe: release skipped, parking loop not found\n");
    return;
  }
  for (UInt32 off = 0x64; off < 0x100; off += 4) {
    if (ml_phys_read(kParkBase + off) != 0) {
      setProperty("ReleaseProbe", "skipped: area 0x64-0xFF not empty");
      IOLog("XenonSMPProbe: release skipped, area 0x64-0xFF not empty\n");
      return;
    }
  }

  ints = ml_set_interrupts_enabled(false);
  for (UInt32 i = 0; i < (sizeof (kProbeLoop) / sizeof (kProbeLoop[0])); i++) {
    ml_phys_write(kProbeCode + (i * 4), kProbeLoop[i]);
  }
  invalidate_icache(kProbeCode, sizeof (kProbeLoop), 1);
  writeCode(kParkEntry, kPPCBranchNext);
  ml_set_interrupts_enabled(ints);

  IODelay(100000);
  publishWords("CountersAfter100ms", kCounterBase, kThreadCount);
  IODelay(100000);
  publishWords("CountersAfter200ms", kCounterBase, kThreadCount);

  // Park the threads again in "b ." at 0x60.
  writeCode(kParkEntry, kPPCBranchSelf);
  IODelay(10000);
  publishWords("CountersParked1", kCounterBase, kThreadCount);
  IODelay(100000);
  publishWords("CountersParked2", kCounterBase, kThreadCount);

  setProperty("ReleaseProbe", "done");
  IOLog("XenonSMPProbe: release probe done\n");
}

bool XenonSMPProbe::start(IOService *provider) {
  if (!super::start(provider)) {
    return false;
  }

  publishWords("Words0x00", kParkBase, 4);
  publishWords("Words0x60", kParkEntry, 8);
  publishWords("Words0xC0", kCounterBase, 8);

  if (getProperty("XenonSMPProbeSPR") == kOSBooleanTrue) {
    runSPRProbe();
  } else if (getProperty("XenonSMPProbeRelease") == kOSBooleanTrue) {
    runReleaseProbe();
  } else {
    setProperty("ReleaseProbe", "not requested");
  }

  registerService();
  return true;
}

void XenonSMPProbe::runSPRProbe(void) {
  UInt32    code[kSPRCodeWords + 1];
  UInt32    savedCode[kSPRCodeWords + 1];
  UInt32    savedRecords[(6 * kRecordSize) / 4];
  UInt64    own[8] __attribute__((aligned(8)));
  UInt32    record[16];
  boolean_t ints;

  if (ml_phys_read(kParkEntry) != kPPCBranchSelf) {
    IOLog("XenonSMPProbe: SPR probe skipped, parking loop not found\n");
    return;
  }

  buildSPRCode(code);
  for (UInt32 i = 0; i <= kSPRCodeWords; i++) {
    savedCode[i] = ml_phys_read(kProbeCode + (i * 4));
  }
  for (UInt32 i = 0; i < (sizeof (savedRecords) / 4); i++) {
    savedRecords[i] = ml_phys_read(kRecordBase + (i * 4));
    ml_phys_write(kRecordBase + (i * 4), 0);
  }

  ints = ml_set_interrupts_enabled(false);
  for (UInt32 i = 0; i <= kSPRCodeWords; i++) {
    ml_phys_write(kProbeCode + (i * 4), code[i]);
  }
  invalidate_icache(kProbeCode, (kSPRCodeWords + 1) * 4, 1);
  writeCode(kParkEntry, kPPCBranchNext);
  ml_set_interrupts_enabled(ints);

  for (UInt32 pass = 0; pass < 2; pass++) {
    IODelay(50000);
    // Read every thread's latest time base back to back, then compare with our own read after
    // them. Each value can only be older than ours, by at most the time the reads take.
    UInt64 theirs[kThreadCount];
    ints = ml_set_interrupts_enabled(false);
    for (UInt32 pir = 1; pir < kThreadCount; pir++) {
      UInt32 tbHi = ml_phys_read(kRecordBase + (pir * kRecordSize) + 56);
      UInt32 tbLo = ml_phys_read(kRecordBase + (pir * kRecordSize) + 60);
      theirs[pir] = (((UInt64) tbHi) << 32) | tbLo;
    }
    readOwnSPRs(own);
    ml_set_interrupts_enabled(ints);
    for (UInt32 pir = 1; pir < kThreadCount; pir++) {
      UInt64 delta = own[7] - theirs[pir];
      IOLog("XenonSMPProbe: pass %lu PIR %lu TB behind PIR 0 by %08lx%08lx ticks\n", (unsigned long) pass,
            (unsigned long) pir, (unsigned long) (delta >> 32), (unsigned long) delta);
    }
  }

  logRecord("PIR0", reinterpret_cast<UInt32*>(own));
  for (UInt32 pir = 1; pir < kThreadCount; pir++) {
    char who[8] = { 'P', 'I', 'R', (char) ('0' + pir), 0 };
    for (UInt32 i = 0; i < 16; i++) {
      record[i] = ml_phys_read(kRecordBase + (pir * kRecordSize) + (i * 4));
    }
    logRecord(who, record);
  }

  // Park the threads, then put back what was there before.
  writeCode(kParkEntry, kPPCBranchSelf);
  IODelay(10000);
  for (UInt32 i = 0; i <= kSPRCodeWords; i++) {
    ml_phys_write(kProbeCode + (i * 4), savedCode[i]);
  }
  invalidate_icache(kProbeCode, (kSPRCodeWords + 1) * 4, 1);
  for (UInt32 i = 0; i < (sizeof (savedRecords) / 4); i++) {
    ml_phys_write(kRecordBase + (i * 4), savedRecords[i]);
  }
  invalidate_icache(kRecordBase, sizeof (savedRecords), 1);

  setProperty("SPRProbe", "done");
  IOLog("XenonSMPProbe: SPR probe done, threads parked, areas restored\n");
}
