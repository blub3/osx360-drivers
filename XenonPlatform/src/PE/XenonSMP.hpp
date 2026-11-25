//
//  XenonSMP.hpp
//  Xbox 360 secondary hardware thread start
//
//  See docs/smp-design.md. Off unless XenonPE personality keys XenonSMPThread1..5 are true.
//

#ifndef XenonSMP_hpp
#define XenonSMP_hpp

#include <IOKit/IOTypes.h>

#define kXenonSMPMaxThreads   6

// Bit n set means hardware thread PIR n runs XNU. Bit 0 is always set.
extern UInt32 gXenonSMPThreadMask;

// With XenonSMPDeferStart, secondary CPUs are registered stopped at boot and started later by
// setting IOCPUState to "Running" on their XenonCPU (IOCPU::setProperties, admin only).
extern bool gXenonSMPDeferStart;

// XNU's IPI handler, from ml_processor_register. Called for the IPI vector on any CPU.
extern void (*gXenonIPIHandler)(void);

UInt32 XenonSMPCurrentPIR(void);
UInt32 XenonSMPLogicalCount(void);
UInt32 XenonSMPLogicalForPIR(UInt32 pir);

// Checks __start_cpu and the OpenBIOS parking area, then installs the release routine and
// the trampoline. Returns false, changing nothing, if anything differs from what was measured.
// With lowPriorityPark, waiting threads run at low thread priority (XenonSMPIdleLow).
bool XenonSMPInstallRelease(UInt32 startCpuAddr, bool lowPriorityPark);

// Single-CPU boot with XenonParkLowPriority: only lowers the priority of the parked threads.
bool XenonSMPInstallLowPriorityPark(void);

// Called from XenonCPU::startCPU after XNU's cpu_start has filled the reset handler.
kern_return_t XenonSMPReleaseThread(UInt32 pir, UInt32 startCpuAddr);

// Replaces cpu_sync_timebase: all six time bases are already synchronized in hardware.
void XenonSMPNoTimebaseSync(void);

// Replaces machine_idle with XenonSMPIdleLow: spins briefly at low thread priority with
// interrupts off, then returns with interrupts on, so an idle CPU leaves its core to the other thread.
extern "C" void XenonSMPIdle(void);

// Kernel addresses resolved by XenonPE at start (0 if not found): PerProcTable and processor_exit.
// processor_exit is not a link-time import of the booting build, so it is called through this.
extern UInt32 gXenonPerProcTable;
extern UInt32 gXenonProcessorExitAddr;

// Called on a secondary thread from quiesceCPU (cpu_sleep). Marks it SleepState and sends it back
// to the mailbox loop, from where startCPU can release it again. Returns only if parking is
// not possible.
void XenonSMPParkSelf(UInt32 logicalCPU);

#endif
