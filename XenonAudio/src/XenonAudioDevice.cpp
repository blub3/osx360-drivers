//
//  XenonAudioDevice.cpp
//  Xbox 360 audio device
//
//  Copyright © 2026 John Davis. All rights reserved.
//

#include "XenonAudioDevice.hpp"
#include "XenonAudioEngine.hpp"
#include "XenonAudio_Regs.hpp"

#include <IOKit/IOUserClient.h>

OSDefineMetaClassAndStructors(XenonAudioDevice, super);

extern "C" vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

//
// Performs driver startup.
// Overrides IOService::start().
//
bool XenonAudioDevice::start(IOService *provider) {
  XenonCheckDebugArgs();

  return super::start(provider);
}

//
// Releases resources used by the driver.
// Overrides IOService::free().
//
void XenonAudioDevice::free(void) {
  OSSafeReleaseNULL(_descBufferDesc);
  OSSafeReleaseNULL(_sampleBufferDesc);
  OSSafeReleaseNULL(_mmioMap);
  OSSafeReleaseNULL(_pciParent);

  super::free();
}

//
// Initializes the audio hardware. Called during super::start().
// Overrides IOAudioDevice::initHardware().
//
bool XenonAudioDevice::initHardware(IOService *provider) {
  XEDBGLOG("Starting Xenon audio controller");

  if (!super::initHardware(provider)) {
    XESYSLOG("super::initHardware() returned false");
    return false;
  }

  _pciParent = OSDynamicCast(IOPCIDevice, provider);
  if (_pciParent == NULL) {
    XESYSLOG("Provider is not IOPCIDevice");
    return false;
  }
  _pciParent->retain();

  // Ensure PCI device is ready.
  _pciParent->setBusMasterEnable(true);
  _pciParent->setMemoryEnable(true);
  _pciParent->setIOEnable(false);

  // Get device properties in order.
  setDeviceName("Built-in Audio");
  setDeviceShortName("Built-in");
  setManufacturerName("Microsoft");
  setProperty(kIOAudioDeviceTransportTypeKey, kIOAudioDeviceTransportTypeBuiltIn, 32);

  // Map in MMIO registers.
  _mmioMap = _pciParent->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
  if (_mmioMap == NULL) {
    XESYSLOG("Failed to map audio registers");
    return false;
  }

  _mmioMem = (volatile UInt8*) _mmioMap->getVirtualAddress();
  XEDBGLOG("Mapped audio registers at 0x%X length 0x%X to %p", _mmioMap->getPhysicalAddress(),
    _mmioMap->getLength(), _mmioMem);

  if (!allocateSampleDescriptors()) {
    XESYSLOG("Failed to allocate sample descriptors");
    return false;
  }
  setupSampleDescriptors();

  _timerEventSource = IOTimerEventSource::timerEventSource(this, OSMemberFunctionCast(IOTimerEventSource::Action, this, &XenonAudioDevice::handleTimer));
  if ((_timerEventSource == NULL) || (workLoop->addEventSource(_timerEventSource) != kIOReturnSuccess)) {
    XESYSLOG("Failed to create timer");
    return false;
  }

  // Optional interrupt-driven updates; falls back to the timer if anything fails.
  _useInterrupt = (getProperty("XenonAudioInterrupt") == kOSBooleanTrue) && setupInterrupt(provider);
  _wantInterrupt = _useInterrupt;
  _positionFix   = _useInterrupt;
  _ackMask       = kXenonAudioRegControlAck;
  _testKnobs     = (getProperty("XenonAudioTestKnobs") == kOSBooleanTrue);

  // Create audio engines for outputs.
  _audioOutputEngine = createAudioEngine(_sampleBuffer, kXenonSampleBufferLength, "Xbox 360 A/V");
  if (_audioOutputEngine == NULL) {
    XESYSLOG("Failed to create audio engine");
    return false;
  }
  if (!createAudioPorts(_audioOutputEngine, kIOAudioOutputPortSubTypeExternalSpeaker, "Xbox 360 A/V")) {
    XESYSLOG("Failed to create audio ports");
    return false;
  }

  // Unmute audio. Xell should have done this but make sure.
  setAudioMute(false);

  // Activate the engines.
  activateAudioEngine(_audioOutputEngine);

  XEDBGLOG("Started Xenon audio controller");
  return true;
}

//
// Starts audio playback on the specified engine.
//
IOReturn XenonAudioDevice::startAudio(XenonAudioEngine *audioEngine) {
  UInt32 reg;
  if (audioEngine == _audioOutputEngine) {
    reg = kXenonAudioRegControl;
  } else {
    return false;
  }
  XEDBGLOG("Start register 0x%X", reg);

  writeReg32(reg, readReg32(reg) | kXenonAudioRegControlRun);
  if (_useInterrupt && _wantInterrupt && !_intStorm) {
    // Make every descriptor but the one before the current valid, then take one interrupt per
    // descriptor. The hardware can only starve if 31 interrupts in a row are missed.
    // Start the wrap detection from the current position, so a restart does not count as a wrap.
    _lastReadPtr = readReg32(kXenonAudioRegState) & kXenonAudioRegStateReadPtrMask;
    updatePosition(true);
    writeReg32(kXenonAudioRegControl, (readReg32(kXenonAudioRegControl) & ~kXenonAudioRegControlStatusMask) | _ackMask);
    _intEventSource->enable();
    _intEnabled = true;
  } else {
    startTimer();
  }

  return kIOReturnSuccess;
}

//
// Stops audio playback on the specified engine.
//
IOReturn XenonAudioDevice::stopAudio(XenonAudioEngine *audioEngine) {
  UInt32 reg;
  if (audioEngine == _audioOutputEngine) {
    reg = kXenonAudioRegControl;
  } else {
    return kIOReturnUnsupported;
  }
  XEDBGLOG("Stop register 0x%X", reg);

  if (_intEnabled) {
    _intEventSource->disable();
    _intEnabled = false;
  }
  _timerEventSource->disable();
  _timerEventSource->cancelTimeout();
  writeReg32(reg, readReg32(reg) & ~(kXenonAudioRegControlRun));

  return kIOReturnSuccess;
}

//
// Gets the current audio playback position.
//
UInt32 XenonAudioDevice::getAudioPosition(XenonAudioEngine *audioEngine) {
  UInt32  reg;
  UInt32  state;
  UInt32  readPtr;
  UInt16  length;
  UInt32  position;

  if (audioEngine == _audioOutputEngine) {
    // The original code reads the control register here; the position is in the state register.
    // The fix is limited to interrupt mode until it has been tested on its own.
    reg = _positionFix ? kXenonAudioRegState : kXenonAudioRegControl;
  } else {
    return kIOReturnUnsupported;
  }

  state    = readReg32(reg);
  readPtr  = state & kXenonAudioRegStateReadPtrMask;
  length   = (UInt16)((state >> kXenonAudioRegStateLengthShift) & 0xFFFF);
  position = (readPtr * (kXenonSampleBufferLength / kXenonAudioDescCount)) + ((kXenonSampleBufferLength / kXenonAudioDescCount) - length);

  return position;
}

//
// Timer handler.
// TODO: In the future the interrupt should be used for this, but right now it just creates a storm.
//
void XenonAudioDevice::handleTimer(IOTimerEventSource *sender) {
  // Get new timestamp if the buffer has looped around.
  UInt32 readPtr = readReg32(kXenonAudioRegState) & kXenonAudioRegStateReadPtrMask;
  if (readPtr == 0 && _lastReadPtr != 0) {
    _audioOutputEngine->takeTimeStamp();
  }
  _lastReadPtr = readPtr;

  if ((readPtr % (kXenonAudioDescCount / 8)) == 0) {
    // Keep the buffer moving.
    readPtr += (kXenonAudioDescCount / 8);
    readPtr &= kXenonAudioDescMask;
    writeReg32(kXenonAudioRegState, readPtr << kXenonAudioRegStateWritePtrShift);
  }

  _timerEventSource->setTimeoutUS(kXenonAudioTimerUpdateUS);
}

//
// Starts the polling timer (default mode, and the fallback after an interrupt storm).
//
void XenonAudioDevice::startTimer(void) {
  _timerEventSource->enable();
  _timerEventSource->setTimeoutUS(kXenonAudioTimerUpdateUS);
}

static inline UInt32 readTimeBaseLower(void) {
  UInt32 tb;
  __asm__ volatile ("mftb %0" : "=r" (tb));
  return tb;
}

//
// Sets up the audio interrupt (PCI bridge IRQ 12, vector 0x44). Stops channel 1 first: firmware
// leaves it stalled with Run set, and that holds the interrupt line active.
//
bool XenonAudioDevice::setupInterrupt(IOService *provider) {
  UInt32 control1 = readReg32(kXenonAudioRegControl1);
  UInt32 tb0;

  if (control1 & kXenonAudioRegControlRun) {
    writeReg32(kXenonAudioRegControl1, control1 & ~kXenonAudioRegControlRun);
  }

  // Time base ticks per second, for the storm guard (no extra kernel imports).
  tb0 = readTimeBaseLower();
  IODelay(10000);
  _ticksPerSecond = (readTimeBaseLower() - tb0) * 100;

  _intEventSource = IOFilterInterruptEventSource::filterInterruptEventSource(this,
    OSMemberFunctionCast(IOInterruptEventSource::Action, this, &XenonAudioDevice::handleInterrupt),
    OSMemberFunctionCast(IOFilterInterruptEventSource::Filter, this, &XenonAudioDevice::filterInterrupt),
    provider, 0);
  if ((_intEventSource == NULL) || (workLoop->addEventSource(_intEventSource) != kIOReturnSuccess)) {
    XESYSLOG("Failed to create audio interrupt, using the timer");
    OSSafeReleaseNULL(_intEventSource);
    return false;
  }

  XESYSLOG("Interrupt-driven position updates enabled (channel 1 control %08X -> %08X, %u ticks/s)",
           control1, readReg32(kXenonAudioRegControl1), _ticksPerSecond);
  return true;
}

//
// Primary interrupt filter, runs in interrupt context. Acknowledges the descriptor interrupt and
// counts interrupts for the storm guard.
//
bool XenonAudioDevice::filterInterrupt(IOFilterInterruptEventSource *source) {
  UInt32 now = readTimeBaseLower();

  writeReg32(kXenonAudioRegControl, (readReg32(kXenonAudioRegControl) & ~kXenonAudioRegControlStatusMask) | _ackMask);
  _intCount++;

  if ((now - _intWindowStart) >= _ticksPerSecond) {
    _intWindowStart = now;
    _intWindowCount = 0;
  }
  if ((++_intWindowCount > kXenonAudioIntStormPerSecond) && !_intStorm) {
    // Soft-disable at the controller; the next interrupt masks the line in the bridge.
    _intStorm = true;
    getProvider()->disableInterrupt(0);
  }
  return true;
}

//
// Secondary interrupt handler, runs on the work loop.
//
void XenonAudioDevice::handleInterrupt(IOInterruptEventSource *source, int count) {
  if (_intStorm) {
    if (_intEnabled) {
      _intEventSource->disable();
      _intEnabled = false;
      XESYSLOG("Audio interrupt storm (%u interrupts), falling back to the timer", _intCount);
      startTimer();
    }
    return;
  }
  updatePosition(true);
}

//
// Takes a time stamp when the read pointer wraps and keeps the hardware supplied. With
// keepAllValid, the write pointer ("last valid descriptor") is set to the descriptor before the
// read pointer; otherwise it moves in steps of a quarter buffer, as the timer always did.
//
void XenonAudioDevice::updatePosition(bool keepAllValid) {
  UInt32 readPtr = readReg32(kXenonAudioRegState) & kXenonAudioRegStateReadPtrMask;

  if (readPtr < _lastReadPtr) {
    _audioOutputEngine->takeTimeStamp();
  }
  _lastReadPtr = readPtr;

  if (keepAllValid) {
    writeReg32(kXenonAudioRegState, ((readPtr - 1) & kXenonAudioDescMask) << kXenonAudioRegStateWritePtrShift);
  }
}

//
// Test switches for listening comparisons, only with the XenonAudioTestKnobs personality key and
// only for administrators. Numbers only (no OSBoolean, which no booting kext imports):
//   XenonAudioAck          acknowledge bits written to control in interrupt mode (0x08 or 0x1C)
//   XenonAudioPositionFix  1 = report the position from the state register, 0 = original code
//   XenonAudioUseInterrupt 1 = interrupt mode, 0 = polling timer; applies at the next engine start
// Overrides IORegistryEntry::setProperties().
//
IOReturn XenonAudioDevice::setProperties(OSObject *properties) {
  OSDictionary  *dict = OSDynamicCast(OSDictionary, properties);
  OSNumber      *number;
  bool          changed = false;

  if (!_testKnobs || (dict == NULL)) {
    return super::setProperties(properties);
  }
  if (IOUserClient::clientHasPrivilege(current_task(), kIOClientPrivilegeAdministrator) != kIOReturnSuccess) {
    return kIOReturnNotPrivileged;
  }

  number = OSDynamicCast(OSNumber, dict->getObject("XenonAudioAck"));
  if (number != NULL) {
    UInt32 mask = number->unsigned32BitValue() & kXenonAudioRegControlAck;
    if ((mask & 0x08) == 0) {
      return kIOReturnBadArgument;   // without bit 3 the descriptor interrupt is never cleared
    }
    _ackMask = mask;
    changed = true;
  }
  number = OSDynamicCast(OSNumber, dict->getObject("XenonAudioPositionFix"));
  if (number != NULL) {
    _positionFix = (number->unsigned32BitValue() != 0);
    changed = true;
  }
  number = OSDynamicCast(OSNumber, dict->getObject("XenonAudioUseInterrupt"));
  if (number != NULL) {
    _wantInterrupt = _useInterrupt && (number->unsigned32BitValue() != 0);
    changed = true;
  }
  if (!changed) {
    return kIOReturnUnsupported;
  }

  XESYSLOG("Test switches: ack %02X, position fix %u, interrupt mode %u (now %s)", _ackMask, _positionFix,
           _wantInterrupt, _intEnabled ? "interrupt" : "timer");
  return kIOReturnSuccess;
}

//
// Mutes or unmutes the console audio.
//
bool XenonAudioDevice::setAudioMute(bool mute) {
  IOService       *xenonSMC;
  mach_timespec_t t;

  t.tv_sec = 30;
  t.tv_nsec = 0;

  xenonSMC = waitForService(serviceMatching("XenonSMC"), &t);
  if (xenonSMC != NULL) {
    return xenonSMC->callPlatformFunction(kXenonFuncSMCMuteAudio, false, (void*)false, NULL, NULL, NULL) == kIOReturnSuccess;
  } else {
    XESYSLOG("SMC services not available");
  }

  return false;
}

//
// Allocates the sample descriptors and sample buffer.
//
bool XenonAudioDevice::allocateSampleDescriptors(void) {
  IOByteCount length;

  // Allocate descriptor buffer. This assumes the buffer is within the first 512MB of memory.
  _descBufferDesc = IOBufferMemoryDescriptor::withOptions(kIOMemoryPhysicallyContiguous, kXenonAudioDescBufferLength, PAGE_SIZE);
  if (_descBufferDesc == NULL) {
    XESYSLOG("Failed to allocate descriptor buffer");
    return false;
  }
  _descBuffer = (UInt32*) _descBufferDesc->getBytesNoCopy();
  _descBufferPhysAddr = _descBufferDesc->getPhysicalSegment(0, &length);

  XEDBGLOG("Allocated descriptor buffer at 0x%X length 0x%X to %p", _descBufferPhysAddr,
    _descBufferDesc->getLength(), _descBuffer);

  // Allocate sample buffer. This assumes the buffer is within the first 512MB of memory.
  _sampleBufferDesc = IOBufferMemoryDescriptor::withOptions(kIOMemoryPhysicallyContiguous, kXenonSampleBufferLength, PAGE_SIZE);
  if (_sampleBufferDesc == NULL) {
    XESYSLOG("Failed to allocate sample buffer");
    return false;
  }

  // Map sample buffer as I/O, audio hardware doesn't seem to be cache coherent.
  // TODO: Is there a better way to do this? Attempting to use IOSetProcessorCache like others doesn't seem to work.
  _sampleBufferPhysAddr = _sampleBufferDesc->getPhysicalSegment(0, &length);
  _sampleBuffer = (void*) ml_io_map(_sampleBufferPhysAddr, kXenonSampleBufferLength);

  XEDBGLOG("Allocated sample buffer at 0x%X length 0x%X to %p", _sampleBufferPhysAddr,
    _sampleBufferDesc->getLength(), _sampleBuffer);

  return true;
}

//
// Fills and configure the sample descriptors.
//
void XenonAudioDevice::setupSampleDescriptors(void) {
  bzero(_sampleBuffer, kXenonSampleBufferLength);

  for (UInt32 i = 0; i < kXenonAudioDescCount; i++) {
    _descBuffer[i * 2]       = OSSwapHostToLittleInt32(_sampleBufferPhysAddr + ((kXenonSampleBufferLength / kXenonAudioDescCount) * i));
    _descBuffer[(i * 2) + 1] = OSSwapHostToLittleInt32(0x80000000 | (kXenonSampleBufferLength / kXenonAudioDescCount));
  }
  flush_dcache((vm_offset_t) _descBuffer, kXenonAudioDescBufferLength, 0);

  writeReg32(kXenonAudioRegControl, 0);
  writeReg32(kXenonAudioRegControl, 0x2000000);
  writeReg32(kXenonAudioRegDescAddr, _descBufferPhysAddr);
  writeReg32(kXenonAudioRegControl, 0x1c08001c);
 // writeReg32(kXenonAudioRegControl, 0x1c00001c);
  writeReg32(kXenonAudioRegFormat, 0x1c);
}

//
// Creates an audio engine.
//
XenonAudioEngine *XenonAudioDevice::createAudioEngine(void *buffer, IOByteCount bufferLength, const char *description) {
  XenonAudioEngine  *audioEngine;
  IOAudioControl    *control;

  // Create a new audio engine with the buffer.
  audioEngine = new XenonAudioEngine;
  if (audioEngine == NULL) {
    return NULL;
  }

  if (!audioEngine->init(this, buffer, bufferLength, description)) {
    OSSafeReleaseNULL(audioEngine);
    return NULL;
  }

  return audioEngine;
}

//
// Creates audio ports for an audio engine.
//
bool XenonAudioDevice::createAudioPorts(XenonAudioEngine *audioEngine, SInt32 type, const char *name) {
  IOAudioPort             *outputPort;
  IOAudioSelectorControl  *outputSelector;
  IOReturn                status;

  outputPort = IOAudioPort::withAttributes(kIOAudioPortTypeOutput, "Output port");
  if (outputPort == NULL) {
    return false;
  }

  outputSelector = IOAudioSelectorControl::createOutputSelector(type, kIOAudioControlChannelIDAll);
  if (outputSelector == NULL) {
    outputPort->release();
    return false;
  }

  // Add selector for nice name in System Preferences.
  audioEngine->addDefaultAudioControl(outputSelector);
  outputSelector->addAvailableSelection(type, name);
  outputSelector->release();

  // Add the port to the engine.
  status = attachAudioPort(outputPort, audioEngine, NULL);
  outputPort->release();

  return status == kIOReturnSuccess;
}
