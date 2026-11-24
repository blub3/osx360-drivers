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

// XNU's IPI handler, from ml_processor_register. Called for the IPI vector on any CPU.
extern void (*gXenonIPIHandler)(void);

UInt32 XenonSMPCurrentPIR(void);
UInt32 XenonSMPLogicalCount(void);
UInt32 XenonSMPLogicalForPIR(UInt32 pir);

// Checks __start_cpu and the OpenBIOS parking area, then installs the release routine and
// the trampoline. Returns false, changing nothing, if anything differs from what was measured.
bool XenonSMPInstallRelease(UInt32 startCpuAddr);

// Called from XenonCPU::startCPU after XNU's cpu_start has filled the reset handler.
kern_return_t XenonSMPReleaseThread(UInt32 pir, UInt32 startCpuAddr);

// Replaces cpu_sync_timebase: all six time bases are already synchronized in hardware.
void XenonSMPNoTimebaseSync(void);

#endif
