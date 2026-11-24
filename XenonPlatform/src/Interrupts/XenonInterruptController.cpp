//
//  XenonInterruptController.cpp
//  Xbox 360 interrupt controller
//
//  Copyright © 2026 John Davis. All rights reserved.
//

#include <ppc/proc_reg.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOPlatformExpert.h>

#include "XenonPE.hpp"
#include "XenonInterruptController.hpp"
#include "XenonSMP.hpp"

OSDefineMetaClassAndStructors(XenonInterruptController, super);

XenonInterruptController *gXenonInterruptController = NULL;

// IPI vector used for all XNU signals (libxenon uses the same one).
#define kXenonIPIVector   kXenonVectorIPI1

//
// Secondary threads only take IPIs. Priority 0 accepts every vector and 0x7C blocks all
// (libxenon), so 0x74 lets only the IPI vector 0x78 through. With priority 0, PIR 2 received the
// Xenos vector 0x58 and hit the panic below (stage 1a, 2026-11-24).
//
#define kXenonSecondaryPriority   0x74

// Unexpected vectors taken by secondary threads, acknowledged and dropped.
static volatile UInt32 sXenonSecondaryStrayVectors = 0;

//
// CPU IRQ to PCI bridge IRQ mapping (filled at startup).
// Used for masking/unmasking.
//
static UInt8 sXenonCPUInterruptMap[kXenonICVectorCount];

//
// Performs driver startup.
// Overrides IOInterruptController::start().
//
bool XenonInterruptController::start(IOService *provider) {
  XenonPE         *xenonPE;
  OSSymbol        *interruptControllerName;
  bool            vectorLockResult;
  IOReturn        status;

  XenonCheckDebugArgs();

  // Get the Xenon platform expert
  xenonPE = OSDynamicCast(XenonPE, getPlatform());
  if (xenonPE == NULL) {
    XESYSLOG("Current platform is not Xenon");
    return false;
  }

  // Fill out interrupt map table.
  for (UInt32 i = 0; i < kXenonICVectorCount; i++) {
    sXenonCPUInterruptMap[i] = kXenonVectorInvalid;
  }
  sXenonCPUInterruptMap[kXenonVectorClock] = 0;
  sXenonCPUInterruptMap[kXenonVectorSATACD] = 1;
  sXenonCPUInterruptMap[kXenonVectorSATAHDD] = 2;
  sXenonCPUInterruptMap[kXenonVectorSMC] = 3;
  sXenonCPUInterruptMap[kXenonVectorOHCI0] = 4;
  sXenonCPUInterruptMap[kXenonVectorEHCI0] = 5;
  sXenonCPUInterruptMap[kXenonVectorOHCI1] = 6;
  sXenonCPUInterruptMap[kXenonVectorEHCI1] = 7;
  sXenonCPUInterruptMap[kXenonVectorEthernet] = 10;
  sXenonCPUInterruptMap[kXenonVectorXMA] = 11;
  sXenonCPUInterruptMap[kXenonVectorAudio] = 12;
  sXenonCPUInterruptMap[kXenonVectorFlash] = 13;

  if (!super::start(provider)) {
    XESYSLOG("super::start() returned false");
    return false;
  }

  interruptControllerName = (OSSymbol *)IODTInterruptControllerName(provider);
  if (interruptControllerName == NULL) {
    XESYSLOG("Failed to get interrupt controller name");
    return false;
  }

  //
  // Map interrupt controller memory.
  // XNU is unable to deal with 64-bit physical MMIO addresses on 32-bit kernels (all PowerPC).
  // Use platform expert hack functions instead.
  //
  status = xenonPE->mapMemory(kXenonICBase, kXenonICLength, (void**) &_mmioMem);
  if (status != kIOReturnSuccess) {
    XESYSLOG("Failed to map interrupt controller memory");
    return false;
  }

  XEDBGLOG("Mapped registers to %p", _mmioMem);

  // Map PCI bridge memory for interrupt routing.
  _bridgeMmioMap = getProvider()->mapDeviceMemoryWithIndex(0);
  if (_bridgeMmioMap == NULL) {
    XESYSLOG("Failed to map bridge memory");
    return false;
  }
  _bridgeMmioMem = (volatile void*) _bridgeMmioMap->getVirtualAddress();
  XEDBGLOG("Mapped bridge memory at 0x%X length 0x%X to %p", _bridgeMmioMap->getPhysicalAddress(),
    _bridgeMmioMap->getLength(), _bridgeMmioMem);

  // Map BIU memory.
  _biuMmioMap = getProvider()->mapDeviceMemoryWithIndex(1);
  if (_biuMmioMap == NULL) {
    XESYSLOG("Failed to map BIU memory");
    return false;
  }
  _biuMmioMem = (volatile void*) _biuMmioMap->getVirtualAddress();
  XEDBGLOG("Mapped BIU memory at 0x%X length 0x%X to %p", _biuMmioMap->getPhysicalAddress(),
    _biuMmioMap->getLength(), _biuMmioMem);

  writeBridgeReg32(0, 0);
  writeBridgeReg32(4, 0x40000000);

  OSWriteLittleInt32(_biuMmioMem, 0x40074, 0x40000000);
  OSWriteLittleInt32(_biuMmioMem, 0x40078, 0xea000050);

  writeBridgeReg32(0xC, 0);
  writeBridgeReg32(0, 0x3);

  // Mask all PCI bridge interrupts if not already.
  for (UInt32 i = 0; i < kXenonPCIBridgeVectorCount; i++) {
    writeBridgeReg32(kXenonPCIBridgeRegIntBase + (i * sizeof(UInt32)), 0);
  }
  eieio();

  // Configure interrupt controller.
  writeICReg64(0, kXenonICRegSpuriousVector, 0x7C);
  writeICReg64(0, kXenonICRegPriority, 0);
  writeICReg64(0, kXenonICRegLogicalID, 1);

  // Purge any pending interrupts.
  while (readICReg64(0, kXenonICRegInterruptAck) != 0x7C) {

  }
 // writeICReg64(0, kXenonICRegEndOfInterruptAutoUpd, 0);
  eieio();

  vectors = (IOInterruptVector *)IOMalloc(kXenonICVectorCount * sizeof (IOInterruptVector));
  if (vectors == NULL) {
    XESYSLOG("Failed to allocate vectors");
    return false;
  }
  bzero(vectors, kXenonICVectorCount * sizeof (IOInterruptVector));

  vectorLockResult = true;
  for (int i = 0; i < kXenonICVectorCount; i++) {
    vectors[i].interruptLock = IOLockAlloc();
    if (vectors[i].interruptLock == NULL) {
      vectorLockResult = false;
      break;
    }
  }
  if (!vectorLockResult) {
    XESYSLOG("Failed to allocate vector locks");
    return false;
  }

  // Secondary CPUs configure their own blocks through this pointer during their start.
  gXenonInterruptController = this;
  registerService();

  //
  // Register this as the platform interrupt controller, once per CPU. registerInterrupt
  // returns only after every CPU has enabled its external interrupt handler.
  //
  getPlatform()->setCPUInterruptProperties(provider);
  for (UInt32 cpu = 0; cpu < XenonSMPLogicalCount(); cpu++) {
    provider->registerInterrupt(cpu, this, getInterruptHandlerAddress(), 0);
    provider->enableInterrupt(cpu);
  }

  getPlatform()->registerInterruptController(interruptControllerName, this);

  XEDBGLOG("Initialized Xenon interrupt controller");
  return true;
}

//
// Gets the address of the primary interrupt handler for this controller.
// Overrides IOInterruptController::getInterruptHandlerAddress().
//
IOInterruptAction XenonInterruptController::getInterruptHandlerAddress(void) {
  return OSMemberFunctionCast(IOInterruptAction, this, &XenonInterruptController::handleInterrupt);
}

//
// Handles all incoming interrupts for this controller and forwards to the appropriate vectors.
// Overrides IOInterruptController::handleInterrupt().
//
IOReturn XenonInterruptController::handleInterrupt(void *refCon, IOService *nub, int source) {
  IOInterruptVector *vector;
  UInt8             vectorIndex;
  UInt8             pciIrq;
  UInt32            pir = XenonSMPCurrentPIR();

  //
  // Get/ack pending IRQ and reset priority, in the block of the thread taking the interrupt.
  //
  vectorIndex = readICReg64(pir, kXenonICRegInterruptAck) & kXenonICRegInterruptAckMask;
  pciIrq = sXenonCPUInterruptMap[vectorIndex];
  writeICReg64(pir, kXenonICRegPriority, 0x7C);
  eieio();
  readICReg64(pir, kXenonICRegPriority);

  //
  // Inter-processor interrupt: hand it to XNU's IPI handler.
  //
  if (vectorIndex == kXenonIPIVector) {
    if (gXenonIPIHandler != NULL) {
      gXenonIPIHandler();
    }
    writeICReg64(pir, kXenonICRegEndOfInterrupt, 0);
    writeICReg64(pir, kXenonICRegEndOfInterruptAutoUpd, 0);
    readICReg64(pir, kXenonICRegPriority);
    return kIOReturnSuccess;
  }

  //
  // Device interrupts are routed to thread 0 only. Drop anything else a secondary receives.
  //
  if (pir != 0) {
    sXenonSecondaryStrayVectors++;
    writeICReg64(pir, kXenonICRegEndOfInterrupt, 0);
    writeICReg64(pir, kXenonICRegEndOfInterruptAutoUpd, 0);
    readICReg64(pir, kXenonICRegPriority);
    return kIOReturnSuccess;
  }

  switch (vectorIndex) {
    case kXenonVectorXenos:
      panic("Got Xenons here!\n");
      break;

    case kXenonVectorIOC:
      printf("Got IOC here\n");
      OSWriteLittleInt32(_biuMmioMem, 0x4002c, 0);
      break;

    case kXenonVectorClock:
      printf("Got the clock here\n");
      break;

    default:
      break;
  }

  if (pciIrq == kXenonVectorInvalid) {
    printf("Got another one here 0x%X\n", vectorIndex);
  }

  vector = &vectors[vectorIndex];
  vector->interruptActive = 1;
  sync();
  isync();

  if (!vector->interruptDisabledSoft) {
    isync();

    // Call the handler if it exists.
    if (vector->interruptRegistered) {
      vector->handler(vector->target, vector->refCon, vector->nub, vector->source);
    }

  } else {
    vector->interruptDisabledHard = 1;
    disableVectorHard(vectorIndex, vector);
  }

  vector->interruptActive = 0;

  // EOI it.
  writeICReg64(pir, kXenonICRegEndOfInterrupt, 0);
  writeICReg64(pir, kXenonICRegEndOfInterruptAutoUpd, 0);
  readICReg64(pir, kXenonICRegPriority);
  return kIOReturnSuccess;
}

//
// Gets the type of vector.
// Overrides IOInterruptController::getVectorType().
//
int XenonInterruptController::getVectorType(IOInterruptVectorNumber vectorNumber, IOInterruptVector *vector) {
  return kIOInterruptTypeLevel;
}

//
// Masks and disables the specified vector.
// Overrides IOInterruptController::disableVectorHard().
//
void XenonInterruptController::disableVectorHard(IOInterruptVectorNumber vectorNumber, IOInterruptVector *vector) {
  UInt8 pciIrq;

  if (vectorNumber >= kXenonICVectorCount) {
    return;
  }

  // Only PCI bridge interrupts can actually be masked.
  pciIrq = sXenonCPUInterruptMap[vectorNumber];
  if (pciIrq == kXenonVectorInvalid) {
    return;
  }

  writeBridgeReg32(kXenonPCIBridgeRegIntBase + (pciIrq * sizeof(UInt32)), 0);
}

//
// Unmasks and enables the specified vector.
// Overrides IOInterruptController::enableVector().
//
void XenonInterruptController::enableVector(IOInterruptVectorNumber vectorNumber, IOInterruptVector *vector) {
  UInt8 pciIrq;
  UInt32 irqState;

  if (vectorNumber >= kXenonICVectorCount) {
    return;
  }

  // Only PCI bridge interrupts can actually be unmasked.
  pciIrq = sXenonCPUInterruptMap[vectorNumber];
  if (pciIrq == kXenonVectorInvalid) {
    return;
  }

  // Configure interrupt to route to CPU0 only.
  irqState = kXenonPCIBridgeRegIntEnabled | (1 << kXenonPCIBridgeRegIntTargetCPUShift) | 0x80 | (vectorNumber >> kXenonPCIBridgeRegIntCPUIRQShift);
  writeBridgeReg32(kXenonPCIBridgeRegIntBase + (pciIrq * sizeof(UInt32)), irqState);
}

//
// Configures a secondary thread's register block like libxenon's wakeup_cpus: spurious vector,
// logical ID (one bit per thread) and a priority that admits only IPIs. Thread 0's block is set
// up in start().
//
void XenonInterruptController::initCPUBlock(UInt32 pir) {
  writeICReg64(pir, kXenonICRegSpuriousVector, 0x7C);
  writeICReg64(pir, kXenonICRegPriority, kXenonSecondaryPriority);
  writeICReg64(pir, kXenonICRegLogicalID, 1ULL << pir);
  eieio();

  // Purge anything pending, with a bound so a bad block cannot hang the CPU here.
  for (UInt32 i = 0; i < 128; i++) {
    if (readICReg64(pir, kXenonICRegInterruptAck) == 0x7C) {
      break;
    }
    writeICReg64(pir, kXenonICRegEndOfInterrupt, 0);
  }
  eieio();
}

//
// Raises the IPI vector on the target thread. The write goes to the sender's block.
//
void XenonInterruptController::sendIPI(UInt32 fromPIR, UInt32 toPIR) {
  writeICReg64(fromPIR, kXenonICRegIPIGeneration,
               ((UInt64) (1U << toPIR) << kXenonICIPITargetShift) | kXenonIPIVector);
  eieio();
}
