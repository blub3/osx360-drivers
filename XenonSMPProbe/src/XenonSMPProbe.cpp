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
};

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

  if (getProperty("XenonSMPProbeRelease") == kOSBooleanTrue) {
    runReleaseProbe();
  } else {
    setProperty("ReleaseProbe", "not requested");
  }

  registerService();
  return true;
}
