//
//  XenosGPUProtocol.h
//  User client interface of XenosGPU (GPU plan step 1.3, docs/gpu-driver-plan.md).
//
//  IOServiceOpen type 0 is the root-only debug client (XenosGPUDebugProtocol.h), type 1 the GPU
//  client for any process. GPU client methods use IOConnectMethodScalarIScalarO. An allocation is
//  mapped with IOConnectMapMemory using its handle as the memory type; the mapping is uncached.
//  Everything a client still holds is freed when it closes or its task exits.
//

#ifndef XenosGPUProtocol_h
#define XenosGPUProtocol_h

enum {
  kXenosGPUDebugClientType      = 0,
  kXenosGPUClientType           = 1
};

enum {
  kXenosGPUAllocate             = 0,   // in: size in bytes, flags (0)  out: handle, GPU address, size
  kXenosGPUFree                 = 1,   // in: handle
  kXenosGPUHeapStatistics       = 2,   // out: heap bytes, free bytes, allocations, largest free block
  kXenosGPUClientMethodCount    = 3
};

enum {
  kXenosGPUPageSize             = 4096,
  kXenosGPUMaxAllocations       = 1024
};

#endif
