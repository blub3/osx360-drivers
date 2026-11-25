//
//  XenonPE.cpp
//  Xbox 360 platform expert
//
//  Copyright © 2026 John Davis. All rights reserved.
//

#include <ppc/proc_reg.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include <IOKit/platform/ApplePlatformExpert.h>
#include <IOKit/IODeviceTreeSupport.h>

#include "XenonPE.hpp"
#include "XenonCPU.hpp"
#include "XenonSMP.hpp"

OSDefineMetaClassAndStructors(XenonPE, super);

#define kMolStdMachineType	1
#define kChipSetTypeMol		170

//
// Performs the platform expert startup.
// Overrides IOService::start().
//
bool XenonPE::start(IOService *provider) {
  IOService       *service;
  IOPMrootDomain  *pmRootDomain;

  XenonCheckDebugArgs();
  XEDBGLOG("Initializing Xenon platform expert");

  if (!mapGPU()) {
    return false;
  }
  startFramebuffer();

  if (!initPatcher()) {
    XESYSLOG("Failed to initialize patcher");
    refreshFramebuffer();
    return false;
  }

  setupSMP();

  setChipSetType(kChipSetTypeMol);
  setMachineType(kMolStdMachineType);
  setBootROMType(kBootROMTypeNewWorld);

  _pePMFeatures     = kStdDesktopPMFeatures;
  _pePrivPMFeatures = kStdDesktopPrivPMFeatures;
  _peNumBatteriesSupported = kStdDesktopNumBatteries;

  if (!super::start(provider)) {
    XESYSLOG("super::start() returned false");
    refreshFramebuffer();
    return false;
  }

  //
  // TODO: Need to implement IONVRAM resources, otherwise XNU waits for 30s for these in IOKitResetTime().
  //
  publishResource("IONVRAM");

  //
  // Prevent sleep/doze, Wii hardware is incapable of sleeping but unsure of doze. Seems to cause issues on Wii U and the GPU.
  //
  service = waitForService(serviceMatching("IOPMrootDomain"));
  pmRootDomain = OSDynamicCast(IOPMrootDomain, service);
  if (pmRootDomain != NULL) {
    pmRootDomain->receivePowerNotification(kIOPMPreventSleep);
  }

  XEDBGLOG("Initialized Xenon platform expert");
  return true;
}

//
// Overrides IODTPlatformExpert::callPlatformFunction().
//
IOReturn XenonPE::callPlatformFunction(const OSSymbol *functionName, bool waitForFunction,
                                       void *param1, void *param2, void *param3, void *param4) {
  if (functionName->isEqualTo(kXenonFuncPlatformStartFB)) {
    startFramebuffer();
    return kIOReturnSuccess;
  } else if (functionName->isEqualTo(kXenonFuncPlatformStopFB)) {
    stopFramebuffer();
    return kIOReturnSuccess;
  }

  return super::callPlatformFunction(functionName, waitForFunction, param1, param2, param3, param4);
}

//
// Overrides IODTPlatformExpert::deleteList()
//
const char *XenonPE::deleteList(void) {
  return("('packages', 'psuedo-usb', 'psuedo-hid', 'multiboot', 'rtas')");
}

//
// Overrides IODTPlatformExpert::excludeList()
//
const char *XenonPE::excludeList(void) {
  //
  // List of DT nodes to exclude from enumeration.
  //
  return("('chosen', 'memory', 'openprom', 'AAPL,ROM', 'rom', 'options', 'aliases')");
}

//
// Overrides IODTPlatformExpert::getMachineName()
//
bool XenonPE::getMachineName(char *name, int maxLength) {
  strncpy(name, "Power Macintosh", maxLength);
  return true;
}

//
// Overrides IODTPlatformExpert::getGMTTimeOfDay().
//
long XenonPE::getGMTTimeOfDay(void) {
  mach_timespec_t t;
  long            secs;

  t.tv_sec = 30;
  t.tv_nsec = 0;
  if (waitForService(resourceMatching("IORTC"), &t) != NULL) {
    if (PE_read_write_time_of_day(kPEReadTOD, &secs) == 0) {
      return secs;
    }
  } else {
    XESYSLOG("RTC did not show up");
  }

  return 0;
}

//
// Optional SMP, see docs/smp-design.md. The XenonPE personality keys XenonSMPThread1..5 select
// extra hardware threads. Without them, or if any check fails, only thread 0 runs, as before.
// Without them, XenonParkLowPriority only lowers the priority of the parked threads.
// Must run before super::start(), which creates the CPU nubs.
//
void XenonPE::setupSMP(void) {
  // cpu_sync_timebase prologue in Tiger 10.4.11; routeFunction relocates these four words.
  static const UInt32 syncPrologue[4] = { 0x7C0802A6, 0xBFC1FFF8, 0x90010008, 0x38600000 };
  // machine_idle prologue in Tiger 10.4.11 (0xAF3A0), replaced with XenonSMPIdleLow.
  static const UInt32 idlePrologue[4] = { 0x7D9142A6, 0x818C0200, 0xA14C0002, 0x816C0060 };
  UInt32  requested = 1;
  UInt32  startCpu;
  UInt32  syncTimebase;
  UInt32  orgSyncTimebase;
  UInt32  machineIdle;
  UInt32  orgMachineIdle;
  bool    idleLow;
  char    key[24];

  for (UInt32 pir = 1; pir < kXenonSMPMaxThreads; pir++) {
    snprintf(key, sizeof (key), "XenonSMPThread%lu", (unsigned long) pir);
    if (getProperty(key) == kOSBooleanTrue) {
      requested |= (1U << pir);
    }
  }
  if (requested == 1) {
    if (getProperty("XenonParkLowPriority") == kOSBooleanTrue) {
      XenonSMPInstallLowPriorityPark();
    }
    return;
  }

  startCpu     = resolveKernelSymbolExact("__start_cpu");
  syncTimebase = resolveKernelSymbolExact("_cpu_sync_timebase");
  if (syncTimebase == 0) {
    XESYSLOG("SMP disabled: cpu_sync_timebase not found");
    return;
  }
  for (UInt32 i = 0; i < 4; i++) {
    if (reinterpret_cast<const UInt32*>(syncTimebase)[i] != syncPrologue[i]) {
      XESYSLOG("SMP disabled: cpu_sync_timebase does not match");
      return;
    }
  }
  idleLow = (getProperty("XenonSMPIdleLow") == kOSBooleanTrue);
  if (!XenonSMPInstallRelease(startCpu, idleLow)) {
    XESYSLOG("SMP disabled: release routine not installed");
    return;
  }
  if (routeFunction(syncTimebase, (UInt32) &XenonSMPNoTimebaseSync, &orgSyncTimebase) != kIOReturnSuccess) {
    XESYSLOG("SMP disabled: cpu_sync_timebase not replaced");
    return;
  }

  // Optional low-priority idle loop; SMP keeps running with Tiger's idle loop if this fails.
  if (idleLow) {
    machineIdle = resolveKernelSymbolExact("_machine_idle");
    for (UInt32 i = 0; (machineIdle != 0) && (i < 4); i++) {
      if (reinterpret_cast<const UInt32*>(machineIdle)[i] != idlePrologue[i]) {
        machineIdle = 0;
      }
    }
    if (machineIdle == 0) {
      XESYSLOG("SMP: machine_idle not found or does not match, idle loop unchanged");
    } else if (routeFunction(machineIdle, (UInt32) &XenonSMPIdle, &orgMachineIdle) != kIOReturnSuccess) {
      XESYSLOG("SMP: machine_idle not replaced, idle loop unchanged");
    } else {
      XESYSLOG("SMP: idle loop at low thread priority");
    }
  }

  // Needed only to stop a secondary CPU again (IOCPUState = "stopped"); optional.
  gXenonPerProcTable      = resolveKernelSymbolExact("_PerProcTable");
  gXenonProcessorExitAddr = resolveKernelSymbolExact("_processor_exit");
  if ((gXenonPerProcTable == 0) || (gXenonProcessorExitAddr == 0)) {
    XESYSLOG("SMP: PerProcTable or processor_exit not found, secondary CPUs cannot be stopped");
  }

  gXenonStartCpuAddr  = startCpu;
  gXenonSMPThreadMask = requested;
  gXenonSMPDeferStart = (getProperty("XenonSMPDeferStart") == kOSBooleanTrue);
  XESYSLOG("SMP enabled for thread mask 0x%X%s", requested, gXenonSMPDeferStart ? ", start deferred" : "");
}
