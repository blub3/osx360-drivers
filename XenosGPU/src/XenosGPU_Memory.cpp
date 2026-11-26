//
//  XenosGPU_Memory.cpp
//  GPU memory manager (GPU plan step 1.3).
//
//  Manages the GPU heap XenonPE publishes (XenosGPUHeapBase/Size, RAM Tiger does not manage,
//  docs/gpu-memory-map.md), minus the first 64 KiB XenosGPU keeps for itself (ring, marker,
//  vertices, fences, boot mark). 4 KiB pages, a bitmap, first fit. Every allocation has a handle, a
//  GPU (physical) address, a size and an owning client; it is zeroed before use, mapped uncached into
//  its owner on request, and freed when the owner frees it, closes, or exits.
//

#include "XenosGPU.hpp"
#include <IOKit/IOPlatformExpert.h>

static const UInt32 kHeapReserved = 0x10000;

IOReturn XenosGPU::initHeap(void) {
  OSNumber *base = OSDynamicCast(OSNumber, getPlatform()->getProperty("XenosGPUHeapBase"));
  OSNumber *size = OSDynamicCast(OSNumber, getPlatform()->getProperty("XenosGPUHeapSize"));
  if (base == NULL || size == NULL || size->unsigned32BitValue() <= kHeapReserved + kXenosGPUPageSize) {
    return kIOReturnNoResources;
  }
  _heapBase = base->unsigned32BitValue() + kHeapReserved;
  _heapPages = (size->unsigned32BitValue() - kHeapReserved) / kXenosGPUPageSize;
  _heapMapBytes = (_heapPages + 7) / 8;
  _pageMap = (UInt8 *)IOMalloc(_heapMapBytes);
  _allocations = (XenosGPUAllocation *)IOMalloc(sizeof(XenosGPUAllocation) * kXenosGPUMaxAllocations);
  _heapLock = IOLockAlloc();
  if (_pageMap == NULL || _allocations == NULL || _heapLock == NULL) {
    freeHeap();
    return kIOReturnNoMemory;
  }
  bzero(_pageMap, _heapMapBytes);
  bzero(_allocations, sizeof(XenosGPUAllocation) * kXenosGPUMaxAllocations);
  _freePages = _heapPages;
  _allocationCount = 0;
  _nextHandle = 1;
  IOLog("XenosGPU: GPU memory manager: 0x%08lx, %lu pages (%lu MiB)\n", (unsigned long)_heapBase,
        (unsigned long)_heapPages, (unsigned long)(_heapPages / 256));
  return kIOReturnSuccess;
}

void XenosGPU::freeHeap(void) {
  if (_allocations != NULL) {
    for (UInt32 i = 0; i < kXenosGPUMaxAllocations; i++) {
      if (_allocations[i].handle != 0 && _allocations[i].memory != NULL) {
        _allocations[i].memory->release();
      }
    }
    IOFree(_allocations, sizeof(XenosGPUAllocation) * kXenosGPUMaxAllocations);
    _allocations = NULL;
  }
  if (_pageMap != NULL) {
    IOFree(_pageMap, _heapMapBytes);
    _pageMap = NULL;
  }
  if (_heapLock != NULL) {
    IOLockFree(_heapLock);
    _heapLock = NULL;
  }
}

static inline bool pageUsed(const UInt8 *map, UInt32 page) {
  return (map[page >> 3] >> (page & 7)) & 1;
}

static inline void markPages(UInt8 *map, UInt32 first, UInt32 count, bool used) {
  for (UInt32 page = first; page < first + count; page++) {
    if (used) {
      map[page >> 3] |= (UInt8)(1 << (page & 7));
    } else {
      map[page >> 3] &= (UInt8)~(1 << (page & 7));
    }
  }
}

IOReturn XenosGPU::allocateMemory(void *owner, UInt32 bytes, UInt32 *handle, UInt32 *gpuAddress,
                                  UInt32 *allocated) {
  if (_allocations == NULL) {
    return kIOReturnNotReady;
  }
  if (bytes == 0 || bytes > _heapPages * kXenosGPUPageSize) {
    return kIOReturnBadArgument;
  }
  UInt32 pages = (bytes + kXenosGPUPageSize - 1) / kXenosGPUPageSize;
  IOLockLock(_heapLock);
  // A free table slot.
  UInt32 slot = kXenosGPUMaxAllocations;
  for (UInt32 i = 0; i < kXenosGPUMaxAllocations; i++) {
    if (_allocations[i].handle == 0) {
      slot = i;
      break;
    }
  }
  // First fit.
  UInt32 first = 0, run = 0;
  bool found = false;
  if (slot != kXenosGPUMaxAllocations && pages <= _freePages) {
    for (UInt32 page = 0; page < _heapPages; page++) {
      if (pageUsed(_pageMap, page)) {
        run = 0;
        continue;
      }
      if (run == 0) {
        first = page;
      }
      if (++run == pages) {
        found = true;
        break;
      }
    }
  }
  if (!found) {
    IOLockUnlock(_heapLock);
    return slot == kXenosGPUMaxAllocations ? kIOReturnNoResources : kIOReturnNoMemory;
  }
  markPages(_pageMap, first, pages, true);
  _freePages -= pages;
  XenosGPUAllocation *allocation = &_allocations[slot];
  allocation->handle = _nextHandle++;
  if (_nextHandle == 0) {
    _nextHandle = 1;
  }
  allocation->phys = _heapBase + first * kXenosGPUPageSize;
  allocation->pages = pages;
  allocation->owner = owner;
  allocation->memory = NULL;
  _allocationCount++;
  IOLockUnlock(_heapLock);

  // Describe and zero it (uncached mapping; plain word stores, no bzero: it uses dcbz).
  IODeviceMemory *memory = IODeviceMemory::withRange(allocation->phys, pages * kXenosGPUPageSize);
  IOMemoryMap *map = memory != NULL ? memory->map(kIOMapInhibitCache) : NULL;
  if (map == NULL) {
    if (memory != NULL) {
      memory->release();
    }
    freeMemory(owner, allocation->handle, true);
    return kIOReturnNoMemory;
  }
  volatile UInt32 *words = (volatile UInt32 *)map->getVirtualAddress();
  for (UInt32 i = 0; i < pages * kXenosGPUPageSize / 4; i++) {
    words[i] = 0;
  }
  asm volatile("sync" ::: "memory");
  map->release();
  allocation->memory = memory;

  *handle = allocation->handle;
  *gpuAddress = allocation->phys;
  *allocated = pages * kXenosGPUPageSize;
  return kIOReturnSuccess;
}

// Refuses (kIOReturnBusy) while the owner still has the allocation mapped, so a client cannot keep
// a mapping of pages that are handed to someone else. force (client close/exit) frees regardless.
IOReturn XenosGPU::freeMemory(void *owner, UInt32 handle, bool force) {
  if (_allocations == NULL || handle == 0) {
    return kIOReturnBadArgument;
  }
  IOLockLock(_heapLock);
  for (UInt32 i = 0; i < kXenosGPUMaxAllocations; i++) {
    XenosGPUAllocation *allocation = &_allocations[i];
    if (allocation->handle != handle) {
      continue;
    }
    if (allocation->owner != owner) {
      IOLockUnlock(_heapLock);
      return kIOReturnNotPermitted;
    }
    if (!force && allocation->memory != NULL && allocation->memory->getRetainCount() > 1) {
      IOLockUnlock(_heapLock);
      return kIOReturnBusy;
    }
    markPages(_pageMap, (allocation->phys - _heapBase) / kXenosGPUPageSize, allocation->pages, false);
    _freePages += allocation->pages;
    _allocationCount--;
    IODeviceMemory *memory = allocation->memory;
    bzero(allocation, sizeof(*allocation));
    IOLockUnlock(_heapLock);
    if (memory != NULL) {
      memory->release();
    }
    return kIOReturnSuccess;
  }
  IOLockUnlock(_heapLock);
  return kIOReturnNotFound;
}

UInt32 XenosGPU::freeAllMemory(void *owner) {
  if (_allocations == NULL) {
    return 0;
  }
  UInt32 freed = 0;
  for (;;) {
    UInt32 handle = 0;
    IOLockLock(_heapLock);
    for (UInt32 i = 0; i < kXenosGPUMaxAllocations; i++) {
      if (_allocations[i].handle != 0 && _allocations[i].owner == owner) {
        handle = _allocations[i].handle;
        break;
      }
    }
    IOLockUnlock(_heapLock);
    if (handle == 0 || freeMemory(owner, handle, true) != kIOReturnSuccess) {
      break;
    }
    freed++;
  }
  return freed;
}

IODeviceMemory *XenosGPU::memoryForHandle(void *owner, UInt32 handle) {
  IODeviceMemory *memory = NULL;
  if (_allocations == NULL || handle == 0) {
    return NULL;
  }
  IOLockLock(_heapLock);
  for (UInt32 i = 0; i < kXenosGPUMaxAllocations; i++) {
    if (_allocations[i].handle == handle && _allocations[i].owner == owner) {
      memory = _allocations[i].memory;
      if (memory != NULL) {
        memory->retain();
      }
      break;
    }
  }
  IOLockUnlock(_heapLock);
  return memory;
}

void XenosGPU::heapStatistics(UInt32 *heapBytes, UInt32 *freeBytes, UInt32 *allocations, UInt32 *largestFree) {
  *heapBytes = *freeBytes = *allocations = *largestFree = 0;
  if (_allocations == NULL) {
    return;
  }
  IOLockLock(_heapLock);
  UInt32 run = 0, largest = 0;
  for (UInt32 page = 0; page < _heapPages; page++) {
    if (pageUsed(_pageMap, page)) {
      run = 0;
    } else if (++run > largest) {
      largest = run;
    }
  }
  *heapBytes = _heapPages * kXenosGPUPageSize;
  *freeBytes = _freePages * kXenosGPUPageSize;
  *allocations = _allocationCount;
  *largestFree = largest * kXenosGPUPageSize;
  IOLockUnlock(_heapLock);
}

//
// Debug: a CP_MEM_WRITE of one word to a GPU address inside the managed heap, under the command gate.
//
IOReturn XenosGPU::gpuWriteAction(OSObject *owner, void *address, void *value, void *, void *) {
  XenosGPU *self = (XenosGPU *)owner;
  if (!self->_presenting) {
    return kIOReturnNotReady;
  }
  self->ringPacket3(CP_MEM_WRITE, 2);
  self->ring((UInt32)(uintptr_t)address | 2);   // 8-in-32 swap, as for the markers
  self->ring((UInt32)(uintptr_t)value);
  self->submit();
  return kIOReturnSuccess;
}

IOReturn XenosGPU::debugGPUWrite(UInt32 address, UInt32 value) {
  if (_gate == NULL || _allocations == NULL || (address & 3) != 0 || address < _heapBase ||
      address >= _heapBase + _heapPages * kXenosGPUPageSize) {
    return kIOReturnBadArgument;
  }
  return _gate->runAction(&XenosGPU::gpuWriteAction, (void *)(uintptr_t)address, (void *)(uintptr_t)value);
}
