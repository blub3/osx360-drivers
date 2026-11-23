//
//  XenonPE_Patcher.cpp
//  Xbox 360 platform expert patching functions
//
//  Copyright © 2026 John Davis. All rights reserved.
//

#include <mach-o/nlist.h>
#include <ppc/proc_reg.h>

#include "XenonPE.hpp"

//
// Kernel map and pmap exported on all versions.
//
extern "C" vm_map_t kernel_map;
extern "C" void *kernel_pmap;

//
// Additional exported functions.
//
extern "C" vm_offset_t pmap_extract(void *pmap, vm_map_offset_t va);
extern "C" kern_return_t vm_protect(vm_map_t map, vm_offset_t start, vm_size_t size, boolean_t set_maximum, vm_prot_t new_protection);
extern "C" unsigned int	ml_phys_read(vm_offset_t paddr);
extern "C" void	ml_phys_write(vm_offset_t paddr, unsigned int data);

// Flags for mapping_make
#define mmFlgBlock		0x80000000	/* This is a block map, use size for number of pages covered */
#define mmFlgUseAttr	0x40000000	/* Use specified attributes */
#define mmFlgPerm		0x20000000	/* Mapping is permanant */
#define mmFlgCInhib		0x00000002	/* Cahching inhibited - use if mapFlgUseAttr set or block */
#define mmFlgGuarded	0x00000001	/* Access guarded - use if mapFlgUseAttr set or block */

XenonPE* XenonPE::_callbackPE = NULL;

//
// Optional dcbz patcher statistics. Disabled unless the xe_pmapstats boot-arg or the
// XenonPmapStats personality property is present. Scan time is in 50 MHz timebase ticks.
//
typedef struct {
  UInt64 pagesScanned;
  UInt64 pagesChanged;
  UInt64 wordsPatched;
  UInt64 scanTicks;
  UInt64 fastNegative;
  UInt64 fastVerified;
  UInt64 fastMismatches;
} XenonPmapStats;

static IOSimpleLock   *sPmapStatsLock = NULL;
static XenonPmapStats sPmapStats;

//
// Optional fast pre-scan, enabled by the xe_pmapfast boot-arg or XenonPmapFastScan property.
// Returns nonzero if a physical page contains a word matching the dcbz patch mask.
// Like bcopy_phys, it runs with data translation off and instruction translation on. External
// interrupts stay off, and the routine must fit in one page so it cannot take an ISI.
//
extern "C" UInt32 xenonPageHasDcbz(UInt32 physAddr);
extern "C" void xenonPageHasDcbzEnd(void);

__asm__(
  ".text\n"
  ".align 6\n"
  ".globl _xenonPageHasDcbz\n"
  "_xenonPageHasDcbz:\n"
  "  mfmsr   r9\n"
  "  lis     r8,0x0200\n"           // MSR[VEC]
  "  ori     r8,r8,0xA010\n"        // MSR[EE|FP|DR]
  "  andc    r10,r9,r8\n"
  "  li      r11,1\n"
  "  rldimi  r10,r11,63,0\n"        // MSR[SF]
  "  rldicl  r3,r3,0,32\n"
  "  addi    r3,r3,-4\n"
  "  li      r4,1024\n"
  "  mtctr   r4\n"
  "  lis     r5,0xFF00\n"
  "  ori     r5,r5,0x07FF\n"
  "  lis     r6,0x7C00\n"
  "  ori     r6,r6,0x07EC\n"
  "  li      r7,0\n"
  "  mtmsrd  r10\n"
  "  isync\n"
  "1:\n"
  "  lwzu    r4,4(r3)\n"
  "  and     r4,r4,r5\n"
  "  cmplw   r4,r6\n"
  "  beq-    2f\n"
  "  bdnz+   1b\n"
  "  b       3f\n"
  "2:\n"
  "  li      r7,1\n"
  "3:\n"
  "  mtmsrd  r9\n"
  "  isync\n"
  "  mr      r3,r7\n"
  "  blr\n"
  ".globl _xenonPageHasDcbzEnd\n"
  "_xenonPageHasDcbzEnd:\n"
  "  blr\n"
);

// Physical pages from 2 GB up need the kernel's I/O access path.
#define kXenonPmapFastScanPageLimit 0x80000

static bool   sPmapFastScanEnabled = false;
static UInt32 sPmapFastScanSample = 0;

//
// Optional patching of code declared through the 32-bit commpage icache flush, used by
// sys_icache_invalidate and MakeDataExecutable. Enabled by the xe_pmapflush boot-arg or the
// XenonPmapFlushPatch property. Code flushed directly with icbi is not covered.
//
// The stub scans the flushed range in user mode and enters the kernel only on a match. It uses
// only registers the original routine clobbers, then resumes it after the replaced instruction.
//
extern "C" void xenonFlushStub(void);
extern "C" void xenonFlushStubEnd(void);

__asm__(
  ".text\n"
  ".align 2\n"
  ".globl _xenonFlushStub\n"
  "_xenonFlushStub:\n"
  "  or.     r4,r4,r4\n"
  "  beq-    3f\n"
  "  rlwinm  r5,r3,0,0,29\n"
  "  add     r6,r3,r4\n"
  "  addi    r6,r6,3\n"
  "  rlwinm  r6,r6,0,0,29\n"
  "1:\n"
  "  lwz     r7,0(r5)\n"
  "  rlwinm  r0,r7,0,21,31\n"
  "  cmplwi  r0,0x7EC\n"
  "  bne+    2f\n"
  "  rlwinm  r0,r7,8,24,31\n"
  "  cmplwi  r0,0x7C\n"
  "  beq-    4f\n"
  "2:\n"
  "  addi    r5,r5,4\n"
  "  cmplw   r5,r6\n"
  "  blt+    1b\n"
  "  b       3f\n"
  "4:\n"
  "  li      r0,0x600E\n"           // Machine call 14
  "  sc\n"
  "3:\n"
  "  or.     r4,r4,r4\n"
  "  .long   0x4BFF8526\n"          // ba 0xFFFF8524
  ".globl _xenonFlushStubEnd\n"
  "_xenonFlushStubEnd:\n"
  "  blr\n"
);

#define kXenonPPCCallIndex          14
#define kXenonCommpageFlushOffset   0x520
#define kXenonCommpageStubOffset    0xC00
#define kXenonCommpageStubMaxSize   0x100
#define kXenonCommpageFlushWord0    0x7C842379  // or. r4,r4,r4
#define kXenonCommpageFlushWord1    0xA0A08026  // lhz r5,0x8026(0)
#define kXenonCommpageFlushBranch   0x4BFF8C02  // ba 0xFFFF8C00
#define kXenonFlushPatchMaxLength   (64U * 1024U * 1024U)
#define kXenonSaveR3Low             0x9C
#define kXenonSaveR4Low             0xA4
#define kXenonSaveSRR1High          0x188

// Install status: 0 not attempted, 1 installed, 2 no commpage or oversized stub,
// 3 kernel code did not match, 4 stub area in use.
static bool             sFlushPatchInstalled = false;
static UInt32           sFlushPatchStatus = 0;
static UInt32           sFlushPatchPpcCalls = 0;
static UInt32           sFlushPatchCommPagePtr = 0;
static volatile SInt32  sFlushPatchCalls = 0;
static volatile SInt32  sFlushPatchWords = 0;
static volatile SInt32  sFlushPatchFailures = 0;

//
// Machine call handler. copyin and copyout honor the caller's permissions and copy-on-write.
// Machine calls enter with interrupts off. A positive return takes the path that restores them.
//
static int xenonFlushPatchCall(void *save) {
  UInt8   *saveBytes = static_cast<UInt8*>(save);
  UInt32  start  = *reinterpret_cast<UInt32*>(saveBytes + kXenonSaveR3Low);
  UInt32  length = *reinterpret_cast<UInt32*>(saveBytes + kXenonSaveR4Low);
  UInt32  words[256];
  UInt32  addr;
  UInt32  end;

  // The hook is only installed in the 32-bit commpage.
  if ((*reinterpret_cast<UInt32*>(saveBytes + kXenonSaveSRR1High) & 0x80000000) != 0) {
    return 1;
  }

  ml_set_interrupts_enabled(TRUE);
  OSIncrementAtomic(const_cast<SInt32*>(&sFlushPatchCalls));

  if ((length == 0) || (length > kXenonFlushPatchMaxLength) || (start > (0xFFFFFFFFU - length - 3))) {
    OSIncrementAtomic(const_cast<SInt32*>(&sFlushPatchFailures));
    return 1;
  }

  addr = start & ~3U;
  end  = (start + length + 3) & ~3U;
  while (addr < end) {
    UInt32 chunk = end - addr;
    if (chunk > sizeof (words)) {
      chunk = sizeof (words);
    }

    if (copyin((user_addr_t) addr, words, chunk) != 0) {
      OSIncrementAtomic(const_cast<SInt32*>(&sFlushPatchFailures));
      break;
    }

    for (UInt32 i = 0; i < (chunk / 4); i++) {
      if ((words[i] & 0xFF0007FF) == 0x7C0007EC) {
        UInt32 patched = words[i] & 0x03FFFFFF;
        if (copyout(&patched, (user_addr_t) (addr + (i * 4)), sizeof (patched)) == 0) {
          OSIncrementAtomic(const_cast<SInt32*>(&sFlushPatchWords));
        } else {
          OSIncrementAtomic(const_cast<SInt32*>(&sFlushPatchFailures));
        }
      }
    }
    addr += chunk;
  }

  return 1;
}

static void installFlushPatch(UInt32 ppcCallsAddr, UInt32 commPagePtrAddr) {
  UInt32    *ppcCalls = reinterpret_cast<UInt32*>(ppcCallsAddr);
  UInt8     *commPage = *reinterpret_cast<UInt8**>(commPagePtrAddr);
  UInt32    *source   = reinterpret_cast<UInt32*>(&xenonFlushStub);
  UInt32    stubSize  = reinterpret_cast<UInt32>(&xenonFlushStubEnd) - reinterpret_cast<UInt32>(&xenonFlushStub);
  UInt32    *entry;
  UInt32    *stub;
  boolean_t ints;

  if ((commPage == NULL) || (stubSize > kXenonCommpageStubMaxSize)) {
    sFlushPatchStatus = 2;
    return;
  }

  entry = reinterpret_cast<UInt32*>(commPage + kXenonCommpageFlushOffset);
  stub  = reinterpret_cast<UInt32*>(commPage + kXenonCommpageStubOffset);

  if ((ppcCalls[kXenonPPCCallIndex] != 0) || (entry[0] != kXenonCommpageFlushWord0) ||
      (entry[1] != kXenonCommpageFlushWord1)) {
    sFlushPatchStatus = 3;
    return;
  }
  for (UInt32 i = 0; i < (kXenonCommpageStubMaxSize / 4); i++) {
    if (stub[i] != 0) {
      sFlushPatchStatus = 4;
      return;
    }
  }

  ints = ml_set_interrupts_enabled(false);

  for (UInt32 i = 0; i < (stubSize / 4); i++) {
    ml_phys_write(pmap_extract(kernel_pmap, (vm_map_offset_t) &stub[i]), source[i]);
  }
  invalidate_icache((vm_offset_t) stub, stubSize, 0);

  ml_phys_write(pmap_extract(kernel_pmap, (vm_map_offset_t) &ppcCalls[kXenonPPCCallIndex]),
                reinterpret_cast<UInt32>(&xenonFlushPatchCall));
  __asm__ volatile ("sync");

  // The single-word entry branch goes in last, once everything it reaches is ready.
  ml_phys_write(pmap_extract(kernel_pmap, (vm_map_offset_t) entry), kXenonCommpageFlushBranch);
  invalidate_icache((vm_offset_t) entry, sizeof (UInt32), 0);

  ml_set_interrupts_enabled(ints);
  sFlushPatchInstalled = true;
  sFlushPatchStatus = 1;
}

//
// The kernel populates the commpage after IOKit starts, so install on the first
// executable user mapping. This runs before any user code can call the routine.
//
static inline void installPendingFlushPatch(void) {
  if (sFlushPatchCommPagePtr != 0) {
    boolean_t ints = ml_set_interrupts_enabled(false);
    UInt32 commPagePtr = sFlushPatchCommPagePtr;

    sFlushPatchCommPagePtr = 0;
    if (commPagePtr != 0) {
      installFlushPatch(sFlushPatchPpcCalls, commPagePtr);
    }
    ml_set_interrupts_enabled(ints);
  }
}

//
// First patched words, with neighbours, for offline code/data classification.
// Entries are immutable once sPmapPatchLogCount includes them.
//
#define kXenonPmapPatchLogEntries 128

typedef struct {
  UInt64 va;
  UInt32 pmap;
  UInt32 word;
  UInt32 previous;
  UInt32 next;
} XenonPmapPatchEntry;

static XenonPmapPatchEntry  sPmapPatchLog[kXenonPmapPatchLogEntries];
static volatile UInt32      sPmapPatchLogCount = 0;

//
// Emulate64 counter. Firmware replaces the three-word diag check at this offset with nops.
// r30 is dead there and the sequence leaves cr0 alone. The counter covers every illegal
// instruction entering the emulator, so it is an upper bound on dcbz traps.
// 0x5210 is in the unused part of the low-memory diag work area.
//
#define kXenonEmulate64DiagOffset   0x5C
#define kXenonEmulationCounterPhys  0x5210
#define kPPCNop                     0x60000000
#define kPPCCmpwiR3Zero             0x2C030000

static bool sEmulationCounterInstalled = false;

static void installEmulationCounter(UInt32 emulate64) {
  static const UInt32 counterCode[3] = {
    0x83C00000 | kXenonEmulationCounterPhys,  // lwz  r30, counter(0)
    0x3BDE0001,                               // addi r30, r30, 1
    0x93C00000 | kXenonEmulationCounterPhys   // stw  r30, counter(0)
  };
  UInt32    *target = reinterpret_cast<UInt32*>(emulate64 + kXenonEmulate64DiagOffset);
  boolean_t ints;

  if ((target[0] != kPPCNop) || (target[1] != kPPCNop) || (target[2] != kPPCNop) ||
      (target[3] != kPPCCmpwiR3Zero) || (ml_phys_read(kXenonEmulationCounterPhys) != 0)) {
    IOLog("XenonPE: Emulate64 did not match, trap counter not installed\n");
    return;
  }

  ints = ml_set_interrupts_enabled(false);
  for (UInt32 i = 0; i < 3; i++) {
    ml_phys_write(pmap_extract(kernel_pmap, (vm_map_offset_t) &target[i]), counterCode[i]);
  }
  invalidate_icache((vm_offset_t) target, sizeof (counterCode), 0);
  ml_set_interrupts_enabled(ints);

  sEmulationCounterInstalled = true;
}

static void recordPatchedWord(void *pmap, UInt64 va, UInt32 word, UInt32 previous, UInt32 next) {
  IOInterruptState state = IOSimpleLockLockDisableInterrupt(sPmapStatsLock);

  if (sPmapPatchLogCount < kXenonPmapPatchLogEntries) {
    XenonPmapPatchEntry *entry = &sPmapPatchLog[sPmapPatchLogCount];

    entry->va       = va;
    entry->pmap     = reinterpret_cast<UInt32>(pmap);
    entry->word     = word;
    entry->previous = previous;
    entry->next     = next;
    sPmapPatchLogCount++;
  }
  IOSimpleLockUnlockEnableInterrupt(sPmapStatsLock, state);
}

static inline UInt64 readTimebase(void) {
  UInt32 upper;
  UInt32 lower;
  UInt32 check;

  do {
    __asm__ volatile ("mftbu %0" : "=r" (upper));
    __asm__ volatile ("mftb %0" : "=r" (lower));
    __asm__ volatile ("mftbu %0" : "=r" (check));
  } while (upper != check);

  return (((UInt64) upper) << 32) | lower;
}

//
// Publishes a current snapshot of the statistics whenever the registry entry is read.
// Overrides IORegistryEntry::serializeProperties().
//
bool XenonPE::serializeProperties(OSSerialize *serialize) const {
  if (sPmapStatsLock != NULL) {
    XenonPE           *self = const_cast<XenonPE*>(this);
    XenonPmapStats    snapshot;
    IOInterruptState  state;

    state = IOSimpleLockLockDisableInterrupt(sPmapStatsLock);
    snapshot = sPmapStats;
    IOSimpleLockUnlockEnableInterrupt(sPmapStatsLock, state);

    self->setProperty("XenonPmapPagesScanned", snapshot.pagesScanned, 64);
    self->setProperty("XenonPmapPagesChanged", snapshot.pagesChanged, 64);
    self->setProperty("XenonPmapWordsPatched", snapshot.wordsPatched, 64);
    self->setProperty("XenonPmapScanTicks", snapshot.scanTicks, 64);
    self->setProperty("XenonPmapFastNegative", snapshot.fastNegative, 64);
    self->setProperty("XenonPmapFastVerified", snapshot.fastVerified, 64);
    self->setProperty("XenonPmapFastMismatches", snapshot.fastMismatches, 64);

    self->setProperty("XenonPmapFlushStatus", sFlushPatchStatus, 32);
    if (sFlushPatchInstalled) {
      self->setProperty("XenonPmapFlushCalls", (UInt32) sFlushPatchCalls, 32);
      self->setProperty("XenonPmapFlushWords", (UInt32) sFlushPatchWords, 32);
      self->setProperty("XenonPmapFlushFailures", (UInt32) sFlushPatchFailures, 32);
    }

    if (sEmulationCounterInstalled) {
      self->setProperty("XenonPmapEmulationTraps", ml_phys_read(kXenonEmulationCounterPhys), 32);
    }

    OSData *patchLog = OSData::withBytes(sPmapPatchLog, sPmapPatchLogCount * sizeof (XenonPmapPatchEntry));
    if (patchLog != NULL) {
      self->setProperty("XenonPmapPatchLog", patchLog);
      patchLog->release();
    }
  }

  return super::serializeProperties(serialize);
}

//
// Search for and get the pointer to the kernel's Mach-O header.
//
bool XenonPE::findKernelMachHeader(void) {
  struct mach_header      *machHeader;
  UInt8                   *machCommands;
  struct load_command     *loadCommand;
  struct segment_command  *segCommand;
  struct segment_command  *linkedItSegCommand;
  struct symtab_command   *symCommand;
  bool                    foundHeader;

  //
  // Search for a valid Mach-O header.
  // On all PowerPC versions, the kernel is mapped 1:1 with no KASLR. The address of the header will vary between versions.
  //
  foundHeader = false;
  for (UInt32 addr = reinterpret_cast<UInt32>(IOLog) & 0xFFFFF000; addr > 0; addr -= PAGE_SIZE) {
    machHeader = reinterpret_cast<struct mach_header*>(addr);
    if ((machHeader->magic == MH_MAGIC) && (machHeader->cputype == CPU_TYPE_POWERPC) && (machHeader->filetype == MH_EXECUTE)) {
      //
      // Found a valid header, check to see if __TEXT segment is first.
      //
      machCommands = reinterpret_cast<UInt8*>(addr) + sizeof (*machHeader);
      for (UInt32 i = 0; i < machHeader->ncmds; i++) {
        loadCommand = reinterpret_cast<struct load_command*>(machCommands);

        if (loadCommand->cmd == LC_SEGMENT) {
          segCommand = reinterpret_cast<struct segment_command*>(loadCommand);
          if (strncmp(segCommand->segname, "__TEXT", sizeof (segCommand->segname)) == 0) {
            foundHeader = true;
            break;
          }
        }

        machCommands += loadCommand->cmdsize;
      }

      if (foundHeader) {
        XEDBGLOG("Found kernel Mach-O header at 0x%X", addr);
        break;
      }
    }
  }

  if (!foundHeader) {
    return false;
  }

  //
  // Get the symbols.
  //
  linkedItSegCommand  = NULL;
  symCommand          = NULL;
  machCommands = reinterpret_cast<UInt8*>(machHeader) + sizeof (*machHeader);
  for (UInt32 i = 0; i < machHeader->ncmds; i++) {
    loadCommand = reinterpret_cast<struct load_command*>(machCommands);

    if (loadCommand->cmd == LC_SEGMENT) {
      segCommand = reinterpret_cast<struct segment_command*>(loadCommand);
      if (strncmp(segCommand->segname, "__LINKEDIT", sizeof (segCommand->segname)) == 0) {
        linkedItSegCommand = segCommand;
      }
    } else if (loadCommand->cmd == LC_SYMTAB) {
      symCommand = reinterpret_cast<struct symtab_command*>(loadCommand);
    }

    machCommands += loadCommand->cmdsize;
  }

  if ((symCommand == NULL) || (linkedItSegCommand == NULL)) {
    return false;
  }

  _symTab           = reinterpret_cast<UInt8*>(linkedItSegCommand->vmaddr + symCommand->symoff - linkedItSegCommand->fileoff);
  _symTabNumSymbols = symCommand->nsyms;
  _strTab           = reinterpret_cast<UInt8*>(linkedItSegCommand->vmaddr + symCommand->stroff - linkedItSegCommand->fileoff);
  _strTabSize       = symCommand->strsize;

  XEDBGLOG("Symtab: %p, strtab: %p", _symTab, _strTab);
  return true;
}

//
// Resolve a kernel symbol. This function can only be called prior to kernel linker jettison.
//
UInt32 XenonPE::resolveKernelSymbol(const char *symbolName) {
  struct nlist  *nlistSym;
  char          *symStr;

  nlistSym = reinterpret_cast<struct nlist*>(_symTab);
  for (UInt32 i = 0; i < _symTabNumSymbols; i++, nlistSym++) {
    //
    // Get the symbol string for the symbol.
    //
    symStr = reinterpret_cast<char*>(_strTab + nlistSym->n_un.n_strx);
    if (strncmp(symbolName, symStr, strlen(symbolName)) == 0) {
      XEDBGLOG("Found symbol '%s' at 0x%X", symbolName, nlistSym->n_value);
      return nlistSym->n_value;
    }
  }

  XESYSLOG("Failed to locate symbol '%s'", symbolName);
  return 0;
}

void XenonPE::wrapPmapEnter(void* pmap, vm_map_offset_t va, ppnum_t pa, vm_prot_t prot, unsigned int flags, boolean_t wired) {
  //
  // Invoke original function.
  //
  reinterpret_cast<void(*)(void*, vm_map_offset_t, ppnum_t, vm_prot_t, unsigned int, boolean_t)>(_callbackPE->_orgPmapEnter)(pmap, va, pa, prot, flags, wired);

  //
  // Perform instruction patching in userspace physical pages.
  //
  if ((pmap != NULL) && (pmap != kernel_pmap) && (prot & VM_PROT_EXECUTE)) {
    installPendingFlushPatch();

    vm_offset_t currPhys = pa << PAGE_SHIFT;
    UInt64 startTicks = (sPmapStatsLock != NULL) ? readTimebase() : 0;
    UInt32 wordsPatched = 0;
    UInt64 stopTicks = 0;
    bool scanPage = true;
    bool fastNegative = false;
    bool fastVerified = false;
    bool fastMismatch = false;

    if (sPmapFastScanEnabled && (pa < kXenonPmapFastScanPageLimit)) {
      scanPage = xenonPageHasDcbz(currPhys) != 0;
      fastNegative = !scanPage;

      //
      // In statistics builds, recheck a sample of clean pages with the original read path.
      //
      if (fastNegative && (sPmapStatsLock != NULL)) {
        stopTicks = readTimebase();
        if ((sPmapFastScanSample++ & 15) == 0) {
          fastVerified = true;
          for (UInt32 i = 0; i < 1024; i++) {
            if ((ml_phys_read(currPhys + (i * 4)) & 0xFF0007FF) == 0x7C0007EC) {
              fastMismatch = true;
              scanPage = true;
              break;
            }
          }
        }
      }
    }

    bool changed = false;
    for (UInt32 i = 0; scanPage && (i < 1024); i++) {
      UInt32 data = ml_phys_read(currPhys);
      if ((data & 0xFF0007FF) == 0x7C0007EC) {
        if (sPmapStatsLock != NULL) {
          recordPatchedWord(pmap, ((UInt64) va) + (i * 4), data,
                            (i > 0) ? ml_phys_read(currPhys - 4) : 0,
                            (i < 1023) ? ml_phys_read(currPhys + 4) : 0);
        }

        data &= 0x03FFFFFF;
        ml_phys_write(currPhys, data);
        changed = true;
        wordsPatched++;
      }

      currPhys += 4;
    }

    if (changed) {
      _callbackPE->_pmapSyncPhysFunc(pa);
    }

    if (sPmapStatsLock != NULL) {
      UInt64 elapsedTicks = ((fastNegative && !fastMismatch) ? stopTicks : readTimebase()) - startTicks;
      IOInterruptState state = IOSimpleLockLockDisableInterrupt(sPmapStatsLock);

      sPmapStats.fastNegative   += fastNegative ? 1 : 0;
      sPmapStats.fastVerified   += fastVerified ? 1 : 0;
      sPmapStats.fastMismatches += fastMismatch ? 1 : 0;
      sPmapStats.pagesScanned++;
      sPmapStats.pagesChanged += changed ? 1 : 0;
      sPmapStats.wordsPatched += wordsPatched;
      sPmapStats.scanTicks    += elapsedTicks;
      IOSimpleLockUnlockEnableInterrupt(sPmapStatsLock, state);
    }
  }
}

void XenonPE::wrapDebugger(const char *str) {
  reinterpret_cast<void(*)(const char*)>(_callbackPE->_orgDebugger)(str);
  _callbackPE->refreshFramebuffer();
}

int XenonPE::wrapGradeBinary(cpu_type_t exectype, cpu_subtype_t execsubtype) {
  switch (execsubtype) {
    case CPU_SUBTYPE_POWERPC_970:
    case CPU_SUBTYPE_POWERPC_7450:
    case CPU_SUBTYPE_POWERPC_7400:
      return 0;
  }

  return reinterpret_cast<int(*)(cpu_type_t, cpu_subtype_t)>(_callbackPE->_orgGradeBinary)(exectype, execsubtype);
}

int XenonPE::wrapGradeCpuSubtype(cpu_subtype_t cpu_subtype) {
  switch (cpu_subtype) {
    case CPU_SUBTYPE_POWERPC_970:
    case CPU_SUBTYPE_POWERPC_7450:
    case CPU_SUBTYPE_POWERPC_7400:
      return 0;
  }

  return reinterpret_cast<int(*)(cpu_subtype_t)>(_callbackPE->_orgGradeCpuSubtype)(cpu_subtype);
}

UInt32 XenonPE::createTrampoline(UInt32 from) {
  UInt32 *fromPtr;
  UInt32 *trampolinePtr;
  UInt32 trampolineBytes[8];

  fromPtr = (UInt32*) from;
  trampolinePtr = &_tmpExecMemory[_tmpExecMemoryOffset];
  _tmpExecMemoryOffset += ARRSIZE(trampolineBytes);

  trampolineBytes[0] = 0x3C000000 | ((from + 16) >> 16);      // lis r0, (from high bytes)
  trampolineBytes[1] = 0x60000000 | ((from + 16) & 0xFFFF);   // ori r0, r0, (from low bytes)
  trampolineBytes[2] = 0x7C0903A6;                            // mtctr r0
  trampolineBytes[3] = fromPtr[0];
  trampolineBytes[4] = fromPtr[1];
  trampolineBytes[5] = fromPtr[2];
  trampolineBytes[6] = fromPtr[3];
  trampolineBytes[7] = 0x4E800420;                            // bctr

  bcopy(trampolineBytes, trampolinePtr, sizeof (trampolineBytes));
  invalidate_icache((vm_offset_t) trampolinePtr, sizeof (trampolineBytes), 0);

  return (UInt32) trampolinePtr;
}

IOReturn XenonPE::routeFunction(UInt32 from, UInt32 to, UInt32 *trampoline) {
  UInt32 *fromPtr;
  UInt32 targetBytes[4];
  boolean_t ints;

  XEDBGLOG("Routing function 0x%X to 0x%X", from, to);

  *trampoline = createTrampoline(from);
  if (*trampoline == 0) {
    XESYSLOG("Failed to creat trampoline for function 0x%X", from);
    return kIOReturnInternalError;
  }
  XEDBGLOG("Trampoline at 0x%X", *trampoline);

  //
  // Build the jump code replacing the prologue of the target function.
  //
  targetBytes[0] = 0x3C000000 | (to >> 16);     // lis r0, (to high bytes)
  targetBytes[1] = 0x60000000 | (to & 0xFFFF);  // ori r0, r0, (from low bytes)
  targetBytes[2] = 0x7C0903A6;                  // mtctr r0
  targetBytes[3] = 0x4E800420;                  // bctr

  //
  // Disable interrupts, write the function bytes, and re-enable.
  //
  ints = ml_set_interrupts_enabled(false);

  fromPtr = (UInt32*) from;
  for (size_t i = 0; i < ARRSIZE(targetBytes); i++) {
    ml_phys_write(pmap_extract(kernel_pmap, (vm_map_offset_t) &fromPtr[i]), targetBytes[i]);
  }
  invalidate_icache((vm_offset_t) fromPtr, sizeof (targetBytes), 0);

  ml_set_interrupts_enabled(ints);

  XEDBGLOG("Routed function 0x%X", from);
  return kIOReturnSuccess;
}

bool XenonPE::initPatcher(void) {
  kern_return_t result;

  _callbackPE = this;

  //
  // Get the kernel header and resolve required non-exported functions.
  //
  if (!findKernelMachHeader()) {
    return false;
  }

  _kmemAllocPageableFunc = (xnu_kmem_alloc_pageable_func) resolveKernelSymbol("_kmem_alloc_pageable");
  _mappingMakeFunc = (xnu_mapping_make_func) resolveKernelSymbol("_mapping_make");
  if (getKernelVersion() >= kKernelVersionTiger) {
    _pmapSyncPhysFunc = (xnu_pmap_sync_phys) resolveKernelSymbol("_pmap_sync_page_data_phys");
  } else {
    _pmapSyncPhysFunc = (xnu_pmap_sync_phys) resolveKernelSymbol("_pmap_sync_caches_phys");
  }

  if ((_kmemAllocPageableFunc == NULL) || (_mappingMakeFunc == NULL) || (_pmapSyncPhysFunc == NULL)) {
    XESYSLOG("Failed to resolve one or more non-exported functions");
    return false;
  }

  result = vm_protect(kernel_map, (vm_address_t) _tmpExecMemory, sizeof(_tmpExecMemory), TRUE, VM_PROT_ALL);
  XEDBGLOG("Adjusted max perms 0x%X", result);
  result = vm_protect(kernel_map, (vm_address_t) _tmpExecMemory, sizeof(_tmpExecMemory), FALSE, VM_PROT_ALL);
  XEDBGLOG("Adjusted new perms 0x%X", result);

  //
  // Statistics must be ready before the pmap_enter hook is installed.
  //
  if (checkKernelArgument("xe_pmapstats") || (getProperty("XenonPmapStats") == kOSBooleanTrue)) {
    sPmapStatsLock = IOSimpleLockAlloc();
    if (sPmapStatsLock == NULL) {
      XESYSLOG("Failed to allocate pmap statistics lock");
    } else {
      UInt32 emulate64 = resolveKernelSymbol("_Emulate64");
      if (emulate64 != 0) {
        installEmulationCounter(emulate64);
      }
    }
  }

  if (checkKernelArgument("xe_pmapfast") || (getProperty("XenonPmapFastScan") == kOSBooleanTrue)) {
    UInt32 scanStart = reinterpret_cast<UInt32>(&xenonPageHasDcbz);
    UInt32 scanEnd   = reinterpret_cast<UInt32>(&xenonPageHasDcbzEnd);

    if ((scanStart & ~0xFFFUL) == (scanEnd & ~0xFFFUL)) {
      sPmapFastScanEnabled = true;
    } else {
      XESYSLOG("Fast pmap scan crosses a page, leaving it disabled");
    }
  }

  if ((getKernelVersion() == kKernelVersionTiger) &&
      (checkKernelArgument("xe_pmapflush") || (getProperty("XenonPmapFlushPatch") == kOSBooleanTrue))) {
    UInt32 ppcCallsAddr    = resolveKernelSymbol("_PPCcalls");
    UInt32 commPagePtrAddr = resolveKernelSymbol("_commPagePtr32");

    if ((ppcCallsAddr != 0) && (commPagePtrAddr != 0)) {
      sFlushPatchPpcCalls    = ppcCallsAddr;
      sFlushPatchCommPagePtr = commPagePtrAddr;
    }
  }

  routeFunction(resolveKernelSymbol("_Debugger"), (UInt32)&XenonPE::wrapDebugger, &_orgDebugger);
  routeFunction(resolveKernelSymbol("_pmap_enter"), (UInt32)&XenonPE::wrapPmapEnter, &_orgPmapEnter);

  if (getKernelVersion() >= kKernelVersionTiger) {
    routeFunction(resolveKernelSymbol("_grade_binary"), (UInt32)&XenonPE::wrapGradeBinary, &_orgGradeBinary);
  } else {
    routeFunction(resolveKernelSymbol("_grade_cpu_subtype"), (UInt32)&XenonPE::wrapGradeCpuSubtype, &_orgGradeCpuSubtype);
  }

  return true;
}

//
// Maps the requested physical address.
// XNU is unable to deal with 64-bit physical addresses normally, hack function to directly map.
//
IOReturn XenonPE::mapMemory(UInt64 physAddr, UInt32 length, void **virtBase) {
  vm_offset_t	  start;
  kern_return_t result;

  //
  // Similar function to io_map in XNU.
  //
  result = _kmemAllocPageableFunc(kernel_map, &start, length);
  if (result != KERN_SUCCESS) {
    return kIOReturnIOError;
  }

  result = _mappingMakeFunc(kernel_pmap, (addr64_t)start, (ppnum_t)(physAddr >> 12),
    (mmFlgBlock | mmFlgUseAttr | mmFlgCInhib | mmFlgGuarded), length >> 12, VM_PROT_READ | VM_PROT_WRITE);
  if (result != KERN_SUCCESS) {
    return kIOReturnIOError;
  }

  *virtBase = (void*)start;
  XEDBGLOG("Mapped 0x%llX to virt 0x%X", physAddr, start);
  return kIOReturnSuccess;
}
