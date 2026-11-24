//
//  XenonCPU.cpp
//  Xbox 360 CPU platform device
//
//  Copyright © 2026 John Davis. All rights reserved.
//

#include <IOKit/IODeviceTreeSupport.h>
#include "XenonCPU.hpp"
#include "XenonPE.hpp"
#include "XenonSMP.hpp"
#include "XenonInterruptController.hpp"

OSDefineMetaClassAndStructors(XenonCPU, super);

static IOCPUInterruptController *gCPUIC;

// Address of __start_cpu, resolved by XenonPE before kernel linker jettison. 0 when SMP is off.
UInt32 gXenonStartCpuAddr = 0;

//
// Overrides IOCPU::start()
//
bool XenonCPU::start(IOService *provider) {
  kern_return_t       result;
  OSData              *tmpData;
  mach_timespec_t     timeout;
  ml_processor_info_t processor_info;

  XenonCheckDebugArgs();

  if (OSDynamicCast(XenonPE, getPlatform()) == NULL) {
    XESYSLOG("Current platform is not Xenon");
    return false;
  }

  if (fromPath("/cpus", gIODTPlane) == NULL) {
    XESYSLOG("Failed to get /cpus from the device tree");
    return false;
  }

  //
  // The device tree has one CPU node, the running thread 0. Secondary threads are XenonCPU
  // instances that the boot CPU creates on the same nub with _isSecondary and _pir set.
  //
  if (!_isSecondary) {
    tmpData = OSDynamicCast(OSData, provider->getProperty("reg"));
    if (tmpData == NULL) {
      XESYSLOG("Failed to read reg property");
      return false;
    }
    _pir = *((UInt32 *)tmpData->getBytesNoCopy());

    tmpData = OSDynamicCast(OSData, provider->getProperty("state"));
    if (tmpData == NULL) {
      XESYSLOG("Failed to read state property");
      return false;
    }
    _isBootCPU = (strcmp((char *)tmpData->getBytesNoCopy(), "running") == 0);
  } else {
    _isBootCPU = false;
  }

  if ((_pir >= kXenonSMPMaxThreads) || (((gXenonSMPThreadMask >> _pir) & 1) == 0)) {
    XESYSLOG("Hardware thread %u is not enabled", _pir);
    return false;
  }

  if (!super::start(provider)) {
    XESYSLOG("super::start() returned false");
    return false;
  }

  _numCPUs = XenonSMPLogicalCount();
  setCPUNumber(XenonSMPLogicalForPIR(_pir));
  XEDBGLOG("PIR %u, logical CPU %lu of %u, boot CPU: %u", _pir, getCPUNumber(), _numCPUs, _isBootCPU);

  if (_isBootCPU) {
    gCPUIC = new IOCPUInterruptController;
    if (gCPUIC == NULL) {
      XESYSLOG("Failed to create IOCPUInterruptController");
      return false;
    }
    if (gCPUIC->initCPUInterruptController(_numCPUs) != kIOReturnSuccess) {
      XESYSLOG("Failed to initialize IOCPUInterruptController");
      return false;
    }
    gCPUIC->attach(this);
  } else {
    //
    // The secondary CPU configures its interrupt controller block during its own start, so
    // wait until the controller has mapped its registers and registered itself.
    //
    timeout.tv_sec  = 30;
    timeout.tv_nsec = 0;
    if ((waitForService(serviceMatching("XenonInterruptController"), &timeout) == NULL) ||
        (gXenonInterruptController == NULL)) {
      XESYSLOG("Interrupt controller did not appear, not starting PIR %u", _pir);
      return false;
    }
  }

  // CPU starts out uninitialized.
  setCPUState(kIOCPUStateUninitalized);

  //
  // Register the CPU with XNU. The boot CPU registers before the CPU interrupt controller is
  // published, so a secondary that waited for the platform controller always comes second.
  //
  processor_info.cpu_id           = (cpu_id_t)this;
  processor_info.boot_cpu         = _isBootCPU;
  processor_info.start_paddr      = 0x0100;
  processor_info.l2cr_value       = 0;
  processor_info.supports_nap     = false;
  processor_info.time_base_enable = NULL;

  result = ml_processor_register(&processor_info, &machProcessor, &ipi_handler);
  if (result == KERN_FAILURE) {
    XESYSLOG("Failed to register the CPU with XNU");
    return false;
  }
  gXenonIPIHandler = ipi_handler;

  if (_isBootCPU) {
    gCPUIC->registerCPUInterruptController();
  }

  if (!_isBootCPU && gXenonSMPDeferStart) {
    // Started later through IOCPU::setProperties (IOCPUState = "Running").
    setCPUState(kIOCPUStateStopped);
    XESYSLOG("PIR %u registered as logical CPU %lu, start deferred", _pir, getCPUNumber());
  } else {
    processor_start(machProcessor);
  }
  registerService();
  XEDBGLOG("Initialized CPU PIR %u", _pir);

  //
  // The boot CPU starts the other enabled threads. Each start returns once that thread has
  // reported in to XNU.
  //
  if (_isBootCPU && (_numCPUs > 1)) {
    for (UInt32 pir = 1; pir < kXenonSMPMaxThreads; pir++) {
      if (((gXenonSMPThreadMask >> pir) & 1) == 0) {
        continue;
      }

      XenonCPU *secondary = new XenonCPU;
      if ((secondary == NULL) || !secondary->init()) {
        XESYSLOG("Failed to create CPU for PIR %u", pir);
        continue;
      }
      secondary->_isSecondary = true;
      secondary->_pir         = pir;
      if (!secondary->attach(provider) || !secondary->start(provider)) {
        XESYSLOG("Failed to start CPU for PIR %u", pir);
        secondary->detach(provider);
      }
      secondary->release();
    }
  }

  return true;
}

//
// Overrides IOCPU::initCPU(). Runs on the CPU being initialized.
//
void XenonCPU::initCPU(bool boot) {
  if (!_isBootCPU && (gXenonInterruptController != NULL)) {
    gXenonInterruptController->initCPUBlock(_pir);
  }

  // Installs this CPU's external interrupt handler in its own per_proc.
  gCPUIC->enableCPUInterrupt(this);

  //
  // The platform controller's registerInterrupt waits until every CPU has enabled its handler.
  // Deferred CPUs enable theirs only when started, so the boot CPU counts them in advance. The
  // extra calls only reinstall the boot CPU's own handler.
  //
  if (_isBootCPU && gXenonSMPDeferStart) {
    for (UInt32 i = 1; i < _numCPUs; i++) {
      gCPUIC->enableCPUInterrupt(this);
    }
  }

  //
  // CPU is now running.
  //
  setCPUState(kIOCPUStateRunning);
}

//
// Overrides IOCPU::signalCPU(). Sends an IPI from the current thread to the target thread.
//
void XenonCPU::signalCPU(IOCPU *target) {
  XenonCPU *targetCPU = OSDynamicCast(XenonCPU, target);

  if ((targetCPU != NULL) && (gXenonInterruptController != NULL)) {
    gXenonInterruptController->sendIPI(XenonSMPCurrentPIR(), targetCPU->_pir);
  }
}

//
// Overrides IOCPU::quiesceCPU()
//
void XenonCPU::quiesceCPU(void) {
  // TODO
}

//
// Overrides IOCPU::startCPU(). Called by XNU's cpu_start after it filled the reset handler.
//
kern_return_t XenonCPU::startCPU(vm_offset_t start_paddr, vm_offset_t arg_paddr) {
  if (_isBootCPU) {
    return KERN_SUCCESS;
  }
  if ((start_paddr != 0x0100) || (gXenonStartCpuAddr == 0)) {
    return KERN_FAILURE;
  }
  return XenonSMPReleaseThread(_pir, gXenonStartCpuAddr);
}

//
// Overrides IOCPU::haltCPU()
//
void XenonCPU::haltCPU(void) {
  // TODO
}

//
// Overrides IOCPU::getCPUName()
//
const OSSymbol *XenonCPU::getCPUName(void) {
  char tmpStr[256];
  snprintf(tmpStr, sizeof (tmpStr), "Primary%lu", getCPUNumber());
  return OSSymbol::withCString(tmpStr);
}
