//
//  XenonSMP.cpp
//  Xbox 360 secondary hardware thread start
//
//  XeLL enters OpenBIOS with threads 1-5 at image offset 0x60. OpenBIOS is linked at 0x07E00000,
//  outside XNU's memory, and parks them there in "b .". This file turns that loop into a
//  mailbox loop. A released thread invalidates its TLB like libxenon does, then enters XNU's
//  reset vector at 0x100. XNU's cpu_start has already pointed the reset handler at 0xF0 to
//  __start_cpu, and XenonSMPReleaseThread redirects it to a copy of __start_cpu that skips the
//  time base writes. The time base is shared and already synchronized (docs/platform-log.md).
//

#include <IOKit/IOLib.h>
#include "XenonSMP.hpp"

extern "C" unsigned int ml_phys_read(vm_offset_t paddr);
extern "C" void ml_phys_write(vm_offset_t paddr, unsigned int data);
extern "C" void invalidate_icache(vm_offset_t addr, unsigned cnt, int phys);
extern "C" boolean_t ml_set_interrupts_enabled(boolean_t enable);

UInt32 gXenonSMPThreadMask = 1;
bool gXenonSMPDeferStart = false;
void (*gXenonIPIHandler)(void) = NULL;

#define kParkBase           0x07E00000
#define kParkEntry          (kParkBase + 0x60)
#define kReleaseCode        (kParkBase + 0x64)
#define kTrampoline         (kParkBase + 0xE00)   // OpenBIOS 0xE00 vector, unused under XNU
#define kMailboxBase        (kParkBase + 0xF80)   // OpenBIOS 0xF00 vector area, one word per PIR

#define kResetHandlerCall   0xF4
#define kResetVector        0x100

#define kPPCBranchSelf      0x48000000            // b .
#define kPPCBranchNext      0x48000004            // b .+4
#define kPPCPriorityLow     0x7C210B78            // or r1,r1,r1: thread priority low
#define kPPCPriorityMedium  0x7C421378            // or r2,r2,r2: thread priority medium (normal)
#define kXeLLArgMagic       0x5F617267            // "_arg", XeLL argument block at image offset 8

// Mailbox loop and TLB invalidation for 0x64-0xC4. The TLB sequence is libxenon's
// startup_from_xell.S. The routine ends in "ba 0x100". The loop branches back to 0x60, which holds
// "b .+4", or "or 1,1,1" with XenonSMPIdleLow so that a waiting thread runs at low priority and
// leaves the core's issue slots to the other thread. A released thread returns to medium priority.
static const UInt32 kReleaseLoop[] = {
  0x7C7FFAA6,   // 0x64 mfspr   r3, PIR
  0x5463103A,   // 0x68 slwi    r3, r3, 2
  0x3C8007E0,   // 0x6C lis     r4, 0x07E0
  0x60840F80,   // 0x70 ori     r4, r4, 0x0F80
  0x7CA4182E,   // 0x74 lwzx    r5, r4, r3
  0x2C050000,   // 0x78 cmpwi   r5, 0
  0x4182FFE4,   // 0x7C beq     0x60
  0x38A00000,   // 0x80 li      r5, 0
  0x7CA4192E,   // 0x84 stwx    r5, r4, r3          (one-shot: clear own mailbox)
  kPPCPriorityMedium,   // 0x88 or r2, r2, r2
  0x7C0004AC,   // 0x8C sync
  0x38600C00,   // 0x90 li      r3, 0xC00
  0x38800100,   // 0x94 li      r4, 0x100
  0x7C8903A6,   // 0x98 mtctr   r4
  0x7C001A24,   // 0x9C tlbiel  r3, 0
  0x38631000,   // 0xA0 addi    r3, r3, 0x1000
  0x4200FFF8,   // 0xA4 bdnz    0x9C
  0x7C0006AC,   // 0xA8 eieio
  0x7C4004AC,   // 0xAC ptesync
  0x386003FF,   // 0xB0 li      r3, 0x3FF
  0x786307C6,   // 0xB4 rldicr  r3, r3, 32, 31
  0x7C201A24,   // 0xB8 tlbiel  r3, 1
  0x7C0006AC,   // 0xBC eieio
  0x7C4004AC,   // 0xC0 ptesync
  0x48000102    // 0xC4 ba      0x100
};

// __start_cpu in Tiger 10.4.11 (mach_kernel MD5 def99c6ef0231ea3c2a627dd26201371).
// Words 11-13 are the time base writes (mttbl r17, mttbu r15, mttbl r16) the trampoline skips.
// Word 14 branches to +0x84, where the trampoline continues.
#define kStartCpuWords        16
#define kStartCpuKeepWords    11
#define kStartCpuContinue     0x84
static const UInt32 kStartCpuExpected[kStartCpuWords] = {
  0x4D4A5182, 0x4D294982, 0x83C30000, 0x83830004, 0x57DE0040, 0x539E003E, 0x3BA3FE0C, 0x7FC3F378,
  0x81E30098, 0x3A200000, 0x8203009C, 0x7E3C43A6, 0x7DFD43A6, 0x7E1C43A6, 0x4800004C, 0x60000000
};

static bool sReleaseInstalled = false;
static UInt32 sParkEntryWord = kPPCBranchNext;

UInt32 XenonSMPCurrentPIR(void) {
  UInt32 pir;
  __asm__ volatile ("mfspr %0, 1023" : "=r" (pir));
  return pir;
}

UInt32 XenonSMPLogicalCount(void) {
  UInt32 count = 0;
  for (UInt32 pir = 0; pir < kXenonSMPMaxThreads; pir++) {
    count += (gXenonSMPThreadMask >> pir) & 1;
  }
  return count;
}

UInt32 XenonSMPLogicalForPIR(UInt32 pir) {
  UInt32 logical = 0;
  for (UInt32 i = 0; i < pir; i++) {
    logical += (gXenonSMPThreadMask >> i) & 1;
  }
  return logical;
}

static void writePhysCode(UInt32 phys, const UInt32 *words, UInt32 count) {
  for (UInt32 i = 0; i < count; i++) {
    ml_phys_write(phys + (i * 4), words[i]);
  }
  invalidate_icache(phys, count * 4, 1);
}

bool XenonSMPInstallRelease(UInt32 startCpuAddr, bool lowPriorityPark) {
  const UInt32  *startCpu = reinterpret_cast<const UInt32*>(startCpuAddr);
  UInt32        trampoline[kStartCpuKeepWords + 1];

  if ((startCpuAddr == 0) || (startCpuAddr > 0x01FFFFFF)) {
    IOLog("XenonSMP: __start_cpu not found or out of branch range\n");
    return false;
  }
  for (UInt32 i = 0; i < kStartCpuWords; i++) {
    if (startCpu[i] != kStartCpuExpected[i]) {
      IOLog("XenonSMP: __start_cpu word %lu is %08lx, expected %08lx\n", (unsigned long) i,
            (unsigned long) startCpu[i], (unsigned long) kStartCpuExpected[i]);
      return false;
    }
  }
  if ((ml_phys_read(kParkEntry) != kPPCBranchSelf) ||
      ((ml_phys_read(kParkBase) & 0xFC000003) != 0x48000000) || (ml_phys_read(kParkBase + 8) != kXeLLArgMagic)) {
    IOLog("XenonSMP: OpenBIOS parking loop not found at 0x%08lx\n", (unsigned long) kParkEntry);
    return false;
  }

  for (UInt32 i = 0; i < kStartCpuKeepWords; i++) {
    trampoline[i] = startCpu[i];
  }
  trampoline[kStartCpuKeepWords] = 0x48000002 | ((startCpuAddr + kStartCpuContinue) & 0x03FFFFFC);  // ba

  for (UInt32 pir = 0; pir < kXenonSMPMaxThreads; pir++) {
    ml_phys_write(kMailboxBase + (pir * 4), 0);
  }
  writePhysCode(kTrampoline, trampoline, kStartCpuKeepWords + 1);
  writePhysCode(kReleaseCode, kReleaseLoop, sizeof (kReleaseLoop) / sizeof (kReleaseLoop[0]));
  __asm__ volatile ("sync");

  // Last: send the parked threads from "b ." into the mailbox loop.
  sParkEntryWord = lowPriorityPark ? kPPCPriorityLow : kPPCBranchNext;
  writePhysCode(kParkEntry, &sParkEntryWord, 1);

  sReleaseInstalled = true;
  IOLog("XenonSMP: release routine installed, thread mask 0x%lx%s\n", (unsigned long) gXenonSMPThreadMask,
        lowPriorityPark ? ", waiting threads at low priority" : "");
  return true;
}

//
// Single-CPU boot (no XenonSMPThread keys): lowers the priority of the five threads XeLL parked in
// OpenBIOS's "b ." at 0x60, which otherwise take issue slots from the running thread on the same
// core (one busy loop 4.58 s, 2.4 s with them at low priority; docs/platform-log.md). 0x64 gets
// "b 0x60" first, then 0x60 becomes "or 1,1,1", so a thread spinning there falls into the
// two-instruction loop. The threads stay parked; nothing else changes.
//
#define kPPCBranchBack      0x4BFFFFFC            // b .-4

bool XenonSMPInstallLowPriorityPark(void) {
  static const UInt32 branchBack = kPPCBranchBack;
  static const UInt32 lowPriority = kPPCPriorityLow;

  if (sReleaseInstalled || (ml_phys_read(kParkEntry) != kPPCBranchSelf) ||
      ((ml_phys_read(kParkBase) & 0xFC000003) != 0x48000000) || (ml_phys_read(kParkBase + 8) != kXeLLArgMagic)) {
    IOLog("XenonSMP: OpenBIOS parking loop not found, waiting threads unchanged\n");
    return false;
  }
  writePhysCode(kReleaseCode, &branchBack, 1);
  __asm__ volatile ("sync");
  writePhysCode(kParkEntry, &lowPriority, 1);
  IOLog("XenonSMP: waiting threads at low priority (single CPU)\n");
  return true;
}

kern_return_t XenonSMPReleaseThread(UInt32 pir, UInt32 startCpuAddr) {
  if (!sReleaseInstalled || (pir == 0) || (pir >= kXenonSMPMaxThreads)) {
    return KERN_FAILURE;
  }

  // cpu_start wrote type 1, __start_cpu and the per_proc address to the reset handler.
  if (ml_phys_read(kResetHandlerCall) != startCpuAddr) {
    IOLog("XenonSMP: reset handler call is %08lx, expected __start_cpu\n",
          (unsigned long) ml_phys_read(kResetHandlerCall));
    return KERN_FAILURE;
  }
  ml_phys_write(kResetHandlerCall, kTrampoline);
  __asm__ volatile ("sync");

  ml_phys_write(kMailboxBase + (pir * 4), 1);
  __asm__ volatile ("sync");

  IOLog("XenonSMP: released PIR %lu\n", (unsigned long) pir);
  return KERN_SUCCESS;
}

void XenonSMPNoTimebaseSync(void) {
}

UInt32 gXenonPerProcTable = 0;
UInt32 gXenonProcessorExitAddr = 0;

// PerProcTable entries in Tiger 10.4.11: 16 bytes, per_proc virtual address at +0x0C
// (cpu_exit_wait). cpu_flags is the halfword at per_proc + 2; SleepState is 0x0800.
#define kPerProcEntrySize   16
#define kPerProcVaddr       0x0C

// cpu_sleep saves the time base at per_proc + 0x1B8 (lower) and + 0x1BC (upper) before calling
// quiesceCPU; cpu_init (0x93018) writes it back on the next start if it is not zero. That set
// CPU 1's time base back by the time it was parked (2026-11-25). The time base keeps running in
// hardware, so the saved value is cleared before parking.
#define kPerProcSaveTBL     0x1B8
#define kPerProcSaveTBU     0x1BC

//
// Parks the calling thread for good: sets SleepState in its per_proc word 0 (cpu_number and
// cpu_flags), which releases cpu_exit_wait on the CPU that asked, then returns to the mailbox
// loop at 0x07E00064 in real mode with the MSR XeLL left on parked threads (measured,
// 0x9000000002003000: 64-bit, hypervisor, ME, FP, VEC). No memory is touched after the flag.
//
extern "C" void xenonSMPPark(UInt32 perProc);

__asm__(
  ".text\n"
  ".align 4\n"
  ".globl _xenonSMPPark\n"
  "_xenonSMPPark:\n"
  "  lis     r5,0x07E0\n"
  "  ori     r5,r5,0x0064\n"
  "  li      r6,0\n"
  "  oris    r6,r6,0x9000\n"
  "  rldicr  r6,r6,32,31\n"
  "  oris    r6,r6,0x0200\n"
  "  ori     r6,r6,0x3000\n"
  "  mtsrr0  r5\n"
  "  mtsrr1  r6\n"
  "  sync\n"
  "1:\n"
  "  lwarx   r4,0,r3\n"
  "  ori     r4,r4,0x0800\n"
  "  stwcx.  r4,0,r3\n"
  "  bne-    1b\n"
  "  sync\n"
  "  rfid\n"
);

void XenonSMPParkSelf(UInt32 logicalCPU) {
  UInt32 perProc;

  if (!sReleaseInstalled || (gXenonPerProcTable == 0) ||
      (ml_phys_read(kParkEntry) != sParkEntryWord)) {
    return;
  }
  perProc = *reinterpret_cast<volatile UInt32*>(gXenonPerProcTable + (logicalCPU * kPerProcEntrySize) +
                                                kPerProcVaddr);
  if ((perProc == 0) || ((perProc & 3) != 0)) {
    return;
  }

  ml_set_interrupts_enabled(FALSE);
  *reinterpret_cast<volatile UInt32*>(perProc + kPerProcSaveTBL) = 0;
  *reinterpret_cast<volatile UInt32*>(perProc + kPerProcSaveTBU) = 0;
  xenonSMPPark(perProc);   // starts with sync, so the stores are visible before SleepState
}

//
// Replaces machine_idle with XenonSMPIdleLow. Tiger cannot nap on Xenon (supports_nap = false), so
// its idle loop spins at medium priority and takes half of the core's issue slots from the other
// hardware thread. This spins about 10 us at low priority with external interrupts off, so the low
// priority never reaches an interrupt handler or another thread, then returns at medium priority
// with interrupts on, like Tiger's machine_idle without nap. Pending interrupts (IPIs, decrementer,
// devices on CPU 0) wait at most this long.
//
#define kIdleSpinTicks      500                   // time base at 50 MHz: 10 us

void XenonSMPIdle(void) {
  UInt32 msr;
  UInt32 start;
  UInt32 now;

  __asm__ volatile ("mfmsr %0" : "=r" (msr));
  __asm__ volatile ("mtmsr %0" : : "r" (msr & ~0x8000U));   // EE off
  __asm__ volatile ("mftb %0" : "=r" (start));
  __asm__ volatile ("or r1,r1,r1");
  do {
    __asm__ volatile ("mftb %0" : "=r" (now));
  } while ((now - start) < kIdleSpinTicks);
  __asm__ volatile ("or r2,r2,r2");
  __asm__ volatile ("mtmsr %0" : : "r" (msr | 0x8000U));    // EE on
}
