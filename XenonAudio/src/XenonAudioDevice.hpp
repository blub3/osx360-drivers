//
//  XenonAudioDevice.hpp
//  Xbox 360 audio device
//
//  Copyright © 2026 John Davis. All rights reserved.
//

#ifndef XenonAudioDevice_hpp
#define XenonAudioDevice_hpp

#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/audio/IOAudioControl.h>
#include <IOKit/audio/IOAudioDefines.h>
#include <IOKit/audio/IOAudioDevice.h>
#include <IOKit/audio/IOAudioPort.h>
#include <IOKit/audio/IOAudioSelectorControl.h>
#include <IOKit/pci/IOPCIDevice.h>

#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOFilterInterruptEventSource.h>
#include "XenonCommon.hpp"

class XenonAudioEngine;

//
// Represents the Xbox 360 audio device driver.
//
class XenonAudioDevice : public IOAudioDevice {
  OSDeclareDefaultStructors(XenonAudioDevice);
  XenonDeclareLogFunctions("auddev");
  typedef IOAudioDevice super;

public:
  // IOService overrides.
  bool start(IOService *provider);
  void free(void);

  // IOAudioDevice overrides.
  bool initHardware(IOService *provider);

  // Test switches (XenonAudioTestKnobs personality key only), see setProperties().
  IOReturn setProperties(OSObject *properties);

  // Audio device functions.
  IOReturn startAudio(XenonAudioEngine *audioEngine);
  IOReturn stopAudio(XenonAudioEngine *audioEngine);
  UInt32 getAudioPosition(XenonAudioEngine *audioEngine);

private:
  IOPCIDevice                   *_pciParent;
  IOMemoryMap                   *_mmioMap;
  volatile void                 *_mmioMem;
  IOTimerEventSource            *_timerEventSource;
  UInt32                        _lastReadPtr;

  // Interrupt-driven position updates (XenonAudioInterrupt personality key). The polling timer
  // stays the default and the fallback.
  IOFilterInterruptEventSource  *_intEventSource;
  bool                          _useInterrupt;
  volatile bool                 _intStorm;
  bool                          _intEnabled;
  UInt32                        _intWindowStart;
  UInt32                        _intWindowCount;
  UInt32                        _ticksPerSecond;
  volatile UInt32               _intCount;

  // Test switches. Defaults: acknowledge 0x1C, position fix and interrupt mode as configured.
  bool                          _testKnobs;
  bool                          _wantInterrupt;
  volatile bool                 _positionFix;
  volatile UInt32               _ackMask;
  volatile bool                 _resetOnStart;

  // Descriptor and sample buffers.
  UInt32                      *_descBuffer;
  IOBufferMemoryDescriptor    *_descBufferDesc;
  IOPhysicalAddress           _descBufferPhysAddr;

  void                        *_sampleBuffer;
  IOBufferMemoryDescriptor    *_sampleBufferDesc;
  IOPhysicalAddress           _sampleBufferPhysAddr;

  // Audio engine.
  XenonAudioEngine    *_audioOutputEngine;

  // Register read/writes.
  inline void writeReg32(UInt32 offset, UInt32 data) {
    OSWriteLittleInt32(_mmioMem, offset, data);
  }
  inline UInt32 readReg32(UInt32 offset) {
    return OSReadLittleInt32(_mmioMem, offset);
  }

  // Internal functions.
  void handleTimer(IOTimerEventSource *sender);
  bool filterInterrupt(IOFilterInterruptEventSource *source);
  void handleInterrupt(IOInterruptEventSource *source, int count);
  void updatePosition(bool keepAllValid);
  bool setupInterrupt(IOService *provider);
  void startTimer(void);
  bool setAudioMute(bool mute);
  bool allocateSampleDescriptors(void);
  void setupSampleDescriptors(void);
  XenonAudioEngine *createAudioEngine(void *buffer, IOByteCount bufferLength, const char *description);
  bool createAudioPorts(XenonAudioEngine *audioEngine, SInt32 type, const char *name);
};

#endif
