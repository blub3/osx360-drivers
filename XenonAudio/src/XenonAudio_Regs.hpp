//
//  XenonAudio_Regs.hpp
//  Xbox 360 audio device registers
//
//  Copyright © 2026 John Davis. All rights reserved.
//

#ifndef XenonAudioRegs_hpp
#define XenonAudioRegs_hpp

// 16-bit PCM in stereo.
#define kXenonAudioNumChannels        2
#define kXenonAudioBitWidth           16
#define kXenonAudioBytesPerFrame      (kXenonAudioNumChannels * (kXenonAudioBitWidth / 8))

#define kXenonAudioTimerUpdateUS      200

#define kXenonAudioDescCount          32
#define kXenonAudioDescMask           0x1F
#define kXenonAudioDescBufferLength   (kXenonAudioDescCount * sizeof (UInt32) * 2)
#define kXenonSampleBufferLength      0x10000

//
// Audio registers.
//
// Audio descriptors base address.
#define kXenonAudioRegDescAddr              0x00
// Audio buffer state.
#define kXenonAudioRegState                 0x04
#define kXenonAudioRegStateReadPtrMask      BITRange(0, 4)
#define kXenonAudioRegStateWritePtrShift    8
#define kXenonAudioRegStateWritePtrMask     BITRange(8, 12)
#define kXenonAudioRegStateLengthShift      16
// Audio control.
#define kXenonAudioRegControl               0x08
#define kXenonAudioRegControlRun            BIT24
// Control bits 2-4 are written as 0x1C at init. Bit 3 reads back as the per-descriptor completion
// status and is cleared by writing 1; writing 0x1C also clears the starved status (measured with
// XenonAudioProbe, docs/audio-interrupt.md).
#define kXenonAudioRegControlStatusMask     0xFF
#define kXenonAudioRegControlAck            0x1C
// Audio format? Analog vs digital.
#define kXenonAudioRegFormat                0x0C

// Second DMA channel at 0x10-0x1C. Firmware leaves it stalled with Run set, which holds the audio
// interrupt line active. XenonAudio does not use it.
#define kXenonAudioRegControl1              0x18

// Interrupt mode: more interrupts than this per second (about 94 are expected) count as a storm.
#define kXenonAudioIntStormPerSecond        1000

#endif
