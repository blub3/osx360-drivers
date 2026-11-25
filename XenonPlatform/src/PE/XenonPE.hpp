//
//  XenonPE.hpp
//  Xbox 360 platform expert
//
//  Copyright © 2026 John Davis. All rights reserved.
//

#ifndef XenonPE_hpp
#define XenonPE_hpp

#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOTimerEventSource.h>
#include <mach-o/loader.h>

#include "XenonCommon.hpp"

#define kXenonPETempExecMemoryInsCount    256

typedef kern_return_t (*xnu_kmem_alloc_pageable_func)(vm_map_t map, vm_offset_t *addrp, vm_size_t size);
typedef addr64_t (*xnu_mapping_make_func)(void *pmap, addr64_t va, ppnum_t pa, unsigned int flags, unsigned int size, vm_prot_t prot);
typedef void (*xnu_pmap_sync_phys)(ppnum_t pa);

//
// Represents the Xbox 360 platform expert.
//
class XenonPE : public IODTPlatformExpert {
  OSDeclareDefaultStructors(XenonPE);
  XenonDeclareLogFunctions("pe");
  typedef IODTPlatformExpert super;

public:
  // IOService overrides.
  bool start(IOService *provider);

  // IORegistryEntry overrides.
  bool serializeProperties(OSSerialize *serialize) const;

  // IOPlatformExpert overrides.
  IOReturn callPlatformFunction(const OSSymbol *functionName, bool waitForFunction,
                                void *param1, void *param2, void *param3, void *param4);
  const char *deleteList(void);
  const char *excludeList(void);
  bool getMachineName(char *name, int maxLength);
  long getGMTTimeOfDay(void);
  IOByteCount savePanicInfo(UInt8 *buffer, IOByteCount length);

  // GPU functions.
  void prepGraphicsFramebuffer(void);
  void refreshFramebuffer(void);
  void startFramebuffer(void);
  void stopFramebuffer(void);

  // Panic screen and GPU interrupt (XenonPE_Panic.cpp).
  IOReturn testPanicScreen(bool begin);
  void silenceGPUInterrupts(void);
  bool gpuInterruptRouting(void) const { return _gpuInterruptRouting; }

  // Patcher functions.
  UInt32 resolveKernelSymbol(const char *symbolName);
  UInt32 resolveKernelSymbolExact(const char *symbolName);
  UInt32 createTrampoline(UInt32 from);
  IOReturn routeFunction(UInt32 from, UInt32 to, UInt32 *trampoline);
  IOReturn mapMemory(UInt64 physAddr, UInt32 length, void **virtBase);

private:
  // GPU.
  IODeviceMemory      *_gpuMmioDeviceMemory;
  IOMemoryMap         *_gpuMmioMap;
  volatile void       *_gpuMmioMem;

  IOPhysicalAddress   _gpuPhysAddr;
  IOPhysicalAddress   _fbPhysAddr;
  IODeviceMemory      *_gpuDeviceMemory;
  IOMemoryMap         *_gpuMap;
  volatile void       *_gpuMem;
  IODeviceMemory      *_fbDeviceMemory;
  IOMemoryMap         *_fbMap;
  volatile void       *_fbMem;

  IOWorkLoop          *_gpuTimerWorkLoop;
  IOTimerEventSource  *_gpuTimerEventSource;

  // Panic screen, panic record and GPU heap.
  bool                _panicScreen;
  bool                _gpuInterruptRouting;
  const UInt8         *_isoFont;
  IOMemoryMap         *_panicRecordMap;

  // Patching/symbol lookups.
  UInt8     *_symTab;
  UInt32    _symTabNumSymbols;
  UInt8     *_strTab;
  UInt32    _strTabSize;
  UInt32    _tmpExecMemory[kXenonPETempExecMemoryInsCount];
  UInt32    _tmpExecMemoryOffset;

  xnu_kmem_alloc_pageable_func  _kmemAllocPageableFunc;
  xnu_mapping_make_func         _mappingMakeFunc;
  xnu_pmap_sync_phys            _pmapSyncPhysFunc;

  // Function hooks.
  static XenonPE    *_callbackPE;
  UInt32            _orgDebugger;
  UInt32            _orgPmapEnter;
  UInt32            _orgGradeBinary;
  UInt32            _orgGradeCpuSubtype;

  // GPU.
  void handleFramebufferTimer(IOTimerEventSource *sender);
  bool mapGPU(void);
  void setupPanicAndHeap(void);
  void panicPixel(UInt32 x, UInt32 y, UInt32 width, UInt32 rowWords, UInt32 argb);
  void drawPanicScreen(const char *title, const char *text);

  // Patching/symbol lookups.
  bool findKernelMachHeader(void);
  bool initPatcher(void);

  // SMP.
  void setupSMP(void);

  // Function hooks.
  static void wrapDebugger(const char *str);
  static void wrapPmapEnter(void* pmap, vm_map_offset_t va, ppnum_t pa, vm_prot_t prot,
                            unsigned int flags, boolean_t wired);
  static int wrapGradeBinary(cpu_type_t exectype, cpu_subtype_t execsubtype);
  static int wrapGradeCpuSubtype(cpu_subtype_t cpu_subtype);
};

#endif
