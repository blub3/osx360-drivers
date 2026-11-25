//
//  XenosGPUProbe.cpp
//  Diagnostic for the Xenos GPU driver plan, step 0.3 (docs/gpu-driver-plan.md).
//
//  Not part of the package and never loaded at boot: load it by hand with kextload while Tiger
//  runs. It checks that a kext can attach to the Xenos PCI device next to XenosController, and
//  how much physically contiguous memory a hand-loaded GPU driver can get. It does not touch any
//  GPU register. Results go to system.log ("XenosGPUProbe: ..."), because ioreg cannot list
//  properties on this system.
//
//  It tries contiguous allocations of 16, 32, 64 and 128 MiB, checks that each one is a single
//  physical run below 512 MiB, and keeps the 64 MiB block until the probe is unloaded.
//
//  It also tests the GPU heap in RAM Tiger does not manage (0x08200000-0x0FFFFFFF; the device tree
//  marks 0x08100000-0x0FFFFFFF available, but OpenBIOS gives Tiger only 0x00000000-0x01FFFFFF and
//  0x10000000-0x1EFFFFFF, and XenonPE keeps a panic record at 0x08100000). The probe records what is there, writes a
//  pattern to every 256th byte through an uncached mapping, reads it back, and checks it again at
//  unload to see whether anything else writes there. Nothing outside that range is written.
//
//  Two switches in the personality, both off: XenosGPUProbePanicScreenTest draws XenonPE's panic
//  screen for 8 s without a panic (profile gpudev). XenosGPUProbePanic panics on purpose to test the
//  real panic screen; the console then hangs and needs the owner (power cycle, XeLL from Aurora).
//

#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <pexpert/pexpert.h>
#include <pexpert/ppc/boot.h>

static const UInt32 kMiB = 1024 * 1024;
static const UInt32 kGPUAddressLimit = 0x20000000;  // Xenos addresses the 512 MiB of system RAM.
static const UInt32 kKeepMiB = 64;
static const UInt32 kHoleBase = 0x08200000;     // GPU heap; 0x08100000-0x081FFFFF holds XenonPE's panic record.
static const UInt32 kHoleSize = 0x07E00000;     // Up to Tiger's second bank at 0x10000000.
static const UInt32 kHoleStride = 256;

class XenosGPUProbe : public IOService {
  OSDeclareDefaultStructors(XenosGPUProbe);
  typedef IOService super;

public:
  bool start(IOService *provider);
  void stop(IOService *provider);

private:
  IOBufferMemoryDescriptor *_kept;
  IOMemoryMap *_holeMap;
  void testHole(void);
  UInt32 checkHole(void);
  void listNeighbours(IOService *provider);
  void listMemoryMap(void);
  void listBootMemory(void);
  IOBufferMemoryDescriptor *tryContiguous(UInt32 mib);
};

OSDefineMetaClassAndStructors(XenosGPUProbe, IOService);

void XenosGPUProbe::listNeighbours(IOService *provider) {
  IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider);
  IOLog("XenosGPUProbe: provider %s (%s), vendor/device 0x%08lx\n", provider->getName(),
        provider->getMetaClass()->getClassName(),
        pci ? (unsigned long)pci->configRead32(kIOPCIConfigVendorID) : 0UL);

  for (UInt32 i = 0; i < provider->getDeviceMemoryCount(); i++) {
    IODeviceMemory *range = provider->getDeviceMemoryWithIndex(i);
    if (range != NULL) {
      IOByteCount length = range->getLength();
      IOLog("XenosGPUProbe: device memory %lu: 0x%08lx, 0x%lx bytes\n", (unsigned long)i,
            (unsigned long)range->getPhysicalAddress(), (unsigned long)length);
    }
  }

  OSIterator *clients = provider->getClientIterator();
  if (clients != NULL) {
    while (OSObject *object = clients->getNextObject()) {
      IOService *client = OSDynamicCast(IOService, object);
      if (client != NULL && client != this) {
        IOLog("XenosGPUProbe: also attached: %s\n", client->getMetaClass()->getClassName());
      }
    }
    clients->release();
  }
}

// Firmware memory map: the device tree's /memory node, read from the kernel because ioreg cannot
// list properties on this system. "reg" is the RAM the firmware reports, "available" what it left free.
void XenosGPUProbe::listMemoryMap(void) {
  IORegistryEntry *memory = IORegistryEntry::fromPath("/memory", gIODTPlane);
  if (memory == NULL) {
    IOLog("XenosGPUProbe: no /memory node\n");
    return;
  }
  static const char *keys[] = { "reg", "available" };
  for (UInt32 k = 0; k < 2; k++) {
    OSData *data = OSDynamicCast(OSData, memory->getProperty(keys[k]));
    if (data == NULL) {
      IOLog("XenosGPUProbe: /memory %s: none\n", keys[k]);
      continue;
    }
    const UInt32 *cells = (const UInt32 *)data->getBytesNoCopy();
    UInt32 count = data->getLength() / sizeof(UInt32);
    IOLog("XenosGPUProbe: /memory %s: %lu bytes\n", keys[k], (unsigned long)data->getLength());
    for (UInt32 i = 0; i + 1 < count && i < 32; i += 2) {
      IOLog("XenosGPUProbe: /memory %s[%lu]: 0x%08lx 0x%08lx\n", keys[k], (unsigned long)(i / 2),
            (unsigned long)cells[i], (unsigned long)cells[i + 1]);
    }
  }
  memory->release();
}

// The DRAM banks OpenBIOS handed to the kernel in the boot arguments: the memory Tiger manages.
// RAM that /memory marks available but no bank covers is used by nobody.
void XenosGPUProbe::listBootMemory(void) {
  const boot_args *args = (const boot_args *)PE_state.bootArgs;
  if (args == NULL) {
    IOLog("XenosGPUProbe: no boot arguments\n");
    return;
  }
  IOLog("XenosGPUProbe: boot args revision %u version %u topOfKernelData 0x%08lx\n",
        args->Revision, args->Version, (unsigned long)args->topOfKernelData);
  UInt32 total = 0;
  for (UInt32 i = 0; i < kMaxDRAMBanks; i++) {
    if (args->PhysicalDRAM[i].size == 0) {
      continue;
    }
    IOLog("XenosGPUProbe: DRAM bank %lu: 0x%08lx 0x%08lx\n", (unsigned long)i,
          (unsigned long)args->PhysicalDRAM[i].base, (unsigned long)args->PhysicalDRAM[i].size);
    total += args->PhysicalDRAM[i].size;
  }
  IOLog("XenosGPUProbe: DRAM banks total 0x%08lx (%lu MiB)\n", (unsigned long)total,
        (unsigned long)(total / kMiB));
}

static inline UInt32 holePattern(UInt32 offset) {
  return (kHoleBase + offset) ^ 0xA5C35A3C;
}

void XenosGPUProbe::testHole(void) {
  _holeMap = NULL;
  IODeviceMemory *range = IODeviceMemory::withRange(kHoleBase, kHoleSize);
  if (range == NULL) {
    IOLog("XenosGPUProbe: hole 0x%08lx: no descriptor\n", (unsigned long)kHoleBase);
    return;
  }
  _holeMap = range->map(kIOMapInhibitCache);
  range->release();
  if (_holeMap == NULL) {
    IOLog("XenosGPUProbe: hole 0x%08lx: map failed\n", (unsigned long)kHoleBase);
    return;
  }
  volatile UInt32 *words = (volatile UInt32 *)_holeMap->getVirtualAddress();

  // What is there now: sample one word per 64 KiB.
  UInt32 nonzero = 0, sum = 0, samples = 0, ones = 0;
  for (UInt32 offset = 0; offset < kHoleSize; offset += 0x10000) {
    UInt32 value = words[offset / 4];
    nonzero += value != 0;
    ones += value == 0xFFFFFFFF;
    sum = (sum << 1 | sum >> 31) ^ value;
    samples++;
  }
  IOLog("XenosGPUProbe: hole 0x%08lx-0x%08lx before: %lu samples, %lu nonzero, %lu all-ones, "
        "hash %08lx\n", (unsigned long)kHoleBase, (unsigned long)(kHoleBase + kHoleSize - 1),
        (unsigned long)samples, (unsigned long)nonzero, (unsigned long)ones, (unsigned long)sum);

  // The pattern survives an unload. Words that still match show that nothing wrote here since
  // an earlier load in the same boot.
  UInt32 kept = (kHoleSize / kHoleStride) - checkHole();
  IOLog("XenosGPUProbe: hole pattern from an earlier load: %lu of %lu words intact\n",
        (unsigned long)kept, (unsigned long)(kHoleSize / kHoleStride));

  AbsoluteTime t0, t1;
  uint64_t ns;
  clock_get_uptime(&t0);
  for (UInt32 offset = 0; offset < kHoleSize; offset += kHoleStride) {
    words[offset / 4] = holePattern(offset);
  }
  clock_get_uptime(&t1);
  SUB_ABSOLUTETIME(&t1, &t0);
  absolutetime_to_nanoseconds(t1, &ns);
  UInt32 mismatches = checkHole();
  IOLog("XenosGPUProbe: hole write %lu words in %lu us, read back: %lu mismatches\n",
        (unsigned long)(kHoleSize / kHoleStride),
        (unsigned long)((ns >> 32) != 0 ? 0xFFFFFFFFU : (UInt32)ns / 1000),
        (unsigned long)mismatches);
}

UInt32 XenosGPUProbe::checkHole(void) {
  volatile UInt32 *words = (volatile UInt32 *)_holeMap->getVirtualAddress();
  UInt32 mismatches = 0, first = 0xFFFFFFFF;
  for (UInt32 offset = 0; offset < kHoleSize; offset += kHoleStride) {
    if (words[offset / 4] != holePattern(offset)) {
      if (mismatches == 0) {
        first = offset;
      }
      mismatches++;
    }
  }
  if (mismatches != 0 && mismatches != kHoleSize / kHoleStride) {
    IOLog("XenosGPUProbe: hole first mismatch at 0x%08lx\n", (unsigned long)(kHoleBase + first));
  }
  return mismatches;
}

IOBufferMemoryDescriptor *XenosGPUProbe::tryContiguous(UInt32 mib) {
  AbsoluteTime t0, t1;
  uint64_t ns;
  clock_get_uptime(&t0);
  IOBufferMemoryDescriptor *buffer = IOBufferMemoryDescriptor::withOptions(
      kIODirectionInOut | kIOMemoryPhysicallyContiguous, mib * kMiB, PAGE_SIZE);
  clock_get_uptime(&t1);
  SUB_ABSOLUTETIME(&t1, &t0);
  absolutetime_to_nanoseconds(t1, &ns);
  // 32-bit division only: the kernel exports no __udivdi3 for a 64-bit one. 0xFFFFFFFF = 4 s or more.
  UInt32 us = (ns >> 32) != 0 ? 0xFFFFFFFFU : (UInt32)ns / 1000;

  if (buffer == NULL) {
    IOLog("XenosGPUProbe: contiguous %lu MiB: FAIL (no memory) after %lu us\n",
          (unsigned long)mib, (unsigned long)us);
    return NULL;
  }

  // Walk the buffer: a contiguous buffer is one physical segment of the full length.
  IOByteCount segLength = 0;
  IOPhysicalAddress base = buffer->getPhysicalSegment(0, &segLength);
  UInt32 segments = 0;
  bool contiguous = true;
  IOPhysicalAddress expected = base;
  for (IOByteCount offset = 0; offset < buffer->getLength(); ) {
    IOByteCount length = 0;
    IOPhysicalAddress phys = buffer->getPhysicalSegment(offset, &length);
    if (phys == 0 || length == 0) {
      contiguous = false;
      break;
    }
    if (phys != expected) {
      contiguous = false;
    }
    segments++;
    expected = phys + length;
    offset += length;
  }
  bool reachable = (base + buffer->getLength()) <= kGPUAddressLimit;

  // Touch every page once so the allocation is real, then check the first and last word.
  volatile UInt32 *words = (volatile UInt32 *)buffer->getBytesNoCopy();
  UInt32 count = buffer->getLength() / sizeof(UInt32);
  for (UInt32 i = 0; i < count; i += PAGE_SIZE / sizeof(UInt32)) {
    words[i] = 0x58454E4F ^ i;  // "XENO"
  }
  bool written = words[0] == 0x58454E4F && words[count - PAGE_SIZE / sizeof(UInt32)] ==
                 (0x58454E4F ^ (count - PAGE_SIZE / sizeof(UInt32)));

  IOLog("XenosGPUProbe: contiguous %lu MiB: %s phys 0x%08lx-0x%08lx segments %lu "
        "contiguous=%d below512MiB=%d written=%d after %lu us\n", (unsigned long)mib,
        (contiguous && reachable && written) ? "PASS" : "FAIL", (unsigned long)base,
        (unsigned long)(base + buffer->getLength() - 1), (unsigned long)segments,
        contiguous, reachable, written, (unsigned long)us);
  return buffer;
}

bool XenosGPUProbe::start(IOService *provider) {
  _kept = NULL;
  _holeMap = NULL;
  if (!super::start(provider)) {
    return false;
  }
  IOLog("XenosGPUProbe: start\n");
  listNeighbours(provider);
  listMemoryMap();
  listBootMemory();

  static const UInt32 sizes[] = { 16, 32, kKeepMiB, 128 };
  for (UInt32 i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
    IOBufferMemoryDescriptor *buffer = tryContiguous(sizes[i]);
    if (buffer == NULL) {
      continue;
    }
    if (sizes[i] == kKeepMiB && _kept == NULL) {
      _kept = buffer;
    } else {
      buffer->release();
    }
  }
  IOLog("XenosGPUProbe: keeping %lu MiB until unload: %s\n", (unsigned long)kKeepMiB,
        _kept != NULL ? "yes" : "no");
  testHole();

  if (getProperty("XenosGPUProbePanicScreenTest") == kOSBooleanTrue) {
    const OSSymbol *begin = OSSymbol::withCString("XenonPanicScreenTest");
    const OSSymbol *end = OSSymbol::withCString("XenonPanicScreenTestEnd");
    IOReturn result = kIOReturnNoMemory;
    if (begin != NULL && end != NULL) {
      result = getPlatform()->callPlatformFunction(begin, true, NULL, NULL, NULL, NULL);
    }
    IOLog("XenosGPUProbe: panic screen test: 0x%08lx\n", (unsigned long)result);
    if (result == kIOReturnSuccess) {
      IOSleep(8000);
      getPlatform()->callPlatformFunction(end, true, NULL, NULL, NULL, NULL);
      IOLog("XenosGPUProbe: panic screen test ended\n");
    }
    if (begin != NULL) {
      begin->release();
    }
    if (end != NULL) {
      end->release();
    }
  }
  if (getProperty("XenosGPUProbePanic") == kOSBooleanTrue) {
    panic("XenosGPUProbe: deliberate panic to test the XenonPE panic screen");
  }

  registerService();
  return true;
}

void XenosGPUProbe::stop(IOService *provider) {
  if (_holeMap != NULL) {
    IOLog("XenosGPUProbe: hole check at unload: %lu mismatches\n", (unsigned long)checkHole());
    _holeMap->release();
    _holeMap = NULL;
  }
  if (_kept != NULL) {
    _kept->release();
    _kept = NULL;
    IOLog("XenosGPUProbe: released %lu MiB\n", (unsigned long)kKeepMiB);
  }
  IOLog("XenosGPUProbe: stop\n");
  super::stop(provider);
}
