//
//  XenosGPUDebugProtocol.h
//  Shared between XenosGPU's debug client and tools/gpu/xenos-gpu.c (GPU plan step 1.2).
//

#ifndef XenosGPUDebugProtocol_h
#define XenosGPUDebugProtocol_h

enum {
  kXenosGPUDebugReadRegister    = 0,   // in: byte offset           out: value
  kXenosGPUDebugWriteRegister   = 1,   // in: byte offset, value    (interrupt registers only)
  kXenosGPUDebugSubmitFence     = 2,   //                           out: sequence
  kXenosGPUDebugWaitFence       = 3,   // in: sequence, timeout ms  out: elapsed us, completed
  kXenosGPUDebugStatistics      = 4,   // out: frames, busy ticks, fences submitted, fence word,
                                       //      interrupts, last CP_INT_STATUS
  kXenosGPUDebugInterrupt       = 5,   // in: 1 register and enable the GPU interrupt, 0 disable
  kXenosGPUDebugMethodCount     = 6
};

enum {
  kXenosGPUFenceMemoryType      = 0    // clientMemoryForType: the fence page, read-only
};

// Fence page layout (32-bit words): word 0 is the last completed fence sequence.
enum {
  kXenosGPUFenceWordSequence    = 0
};

#endif
