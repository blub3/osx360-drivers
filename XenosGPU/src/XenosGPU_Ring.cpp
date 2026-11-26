//
//  XenosGPU_Ring.cpp
//  Command ring, firmware and display state (GPU plan step 1.1).
//
//  The sequences follow XenonCoreGraphics 0.0.5d46-0.0.5d48 (XenosController_Setup.cpp,
//  XenosController_Firmware.cpp), which started the ring reliably on hardware: halt, soft reset,
//  halt again, reset both ring pointers, load and verify the firmware, start the micro engine,
//  initialize it, check a MEM_WRITE marker, set up the display state, check a second marker.
//

#include "XenosGPU.hpp"
#include "../../XenonCoreGraphics/src/XenosFirmware.hpp"
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceMemory.h>

#define kCacheLine            128

static const UInt32 kCpMeHalt         = 1U << 28;
static const UInt32 kCpMeRunControl   = 0x0000FFFF;
static const UInt32 kCpRingControl    = 0x0802000C;
static const UInt32 kCpAllowRptrWrite = 1U << 31;
static const UInt32 kStartupAttempts  = 3;
static const UInt32 kDrainPolls       = 5000;
static const UInt32 kRingBytes        = kXenosRingbufferLength * sizeof(UInt32);
// The two display buffers occupy the top 16 MiB of physical RAM.
static const IOPhysicalAddress kDisplayMemoryStart = 0x20000000 - 2 * kXenosFramebufferLength;

static const UInt32 kMarkerInitial    = 0xA5A5A5A5;
static const UInt32 kMarkerExpected   = 0x58454E4F;
static const UInt32 kMarkerSwap8In32  = 2;
static const UInt32 kMarkerPolls      = 2000;

static const float sTexCoords[] = {
  0.0f, 1.0f,
  1.0f, 1.0f,
  0.0f, 0.0f,
  1.0f, 0.0f,
};

static const float sQuad[] = {
  -1.0f, -1.0f, 0.0f,
  1.0f, -1.0f, 0.0f,
  -1.0f, 1.0f, 0.0f,
  1.0f, 1.0f, 0.0f
};

static inline UInt32 xy32(UInt32 x, UInt32 y) {
  return x | (y << 16);
}

static void flushRange(volatile void *data, UInt32 length) {
  uintptr_t base = (uintptr_t)data & ~((uintptr_t)kCacheLine - 1);
  uintptr_t end = (uintptr_t)data + length;
  for (; base < end; base += kCacheLine) {
    asm volatile("dcbst 0,%0" : : "r"(base) : "memory");
  }
  asm volatile("sync" ::: "memory");
}

static inline void flushLine(volatile UInt32 *slot) {
  asm volatile("dcbf 0,%0\n\tsync" : : "r"(slot) : "memory");
}

static IOBufferMemoryDescriptor *contiguousPage(const void *bytes, UInt32 length, IOPhysicalAddress *phys) {
  IOBufferMemoryDescriptor *desc = IOBufferMemoryDescriptor::withOptions(kIOMemoryPhysicallyContiguous,
                                                                         PAGE_SIZE, PAGE_SIZE);
  if (desc == NULL) {
    return NULL;
  }
  IOByteCount segment = 0;
  *phys = desc->getPhysicalSegment(0, &segment);
  if (bytes != NULL) {
    desc->writeBytes(0, bytes, length);
  }
  flushRange(desc->getBytesNoCopy(), PAGE_SIZE);
  return desc;
}

void XenosGPU::ringConstant(UInt32 reg, UInt32 value) {
  ringPacket3(CP_SET_CONSTANT, 2);
  ring(XENOS_CP_REG(reg));
  ring(value);
}

//
// GPU memory for the ring, the marker and the vertex buffers. By default it comes from the GPU heap
// XenonPE publishes (XenosGPUHeapBase, RAM Tiger does not manage, docs/gpu-memory-map.md), mapped
// uncached: XenonCoreGraphics, which ran stably, always had its ring below 32 MiB, while runtime
// allocations from Tiger land at 0x1A000000 and above, where presenting stalled the GPU after about
// 1 s (2026-11-25). The personality key XenosGPUTigerMemory selects the old allocations for comparison.
// No bzero or memcpy on the uncached mapping: they use dcbz, which faults there.
//
static const UInt32 kHeapRing      = 0x0000;
static const UInt32 kHeapMarker    = 0x8000;
static const UInt32 kHeapTexCoords = 0x9000;
static const UInt32 kHeapQuad      = 0xA000;
static const UInt32 kHeapUsed      = 0x10000;

static void copyWords(volatile UInt8 *dst, const void *src, UInt32 length) {
  const UInt32 *words = (const UInt32 *)src;
  for (UInt32 i = 0; i < length / 4; i++) {
    ((volatile UInt32 *)dst)[i] = words[i];
  }
}

IOReturn XenosGPU::allocateHeapBuffers(void) {
  OSNumber *base = OSDynamicCast(OSNumber, getPlatform()->getProperty("XenosGPUHeapBase"));
  OSNumber *size = OSDynamicCast(OSNumber, getPlatform()->getProperty("XenosGPUHeapSize"));
  if (base == NULL || size == NULL || size->unsigned32BitValue() < kHeapUsed) {
    IOLog("XenosGPU: no GPU heap published (boot profile gpudev publishes it)\n");
    return kIOReturnNoResources;
  }
  IOPhysicalAddress phys = base->unsigned32BitValue();
  IODeviceMemory *memory = IODeviceMemory::withRange(phys, kHeapUsed);
  if (memory != NULL) {
    _heapMap = memory->map(kIOMapInhibitCache);
    memory->release();
  }
  if (_heapMap == NULL) {
    return kIOReturnNoMemory;
  }
  volatile UInt8 *virt = (volatile UInt8 *)_heapMap->getVirtualAddress();
  _ring = virt + kHeapRing;
  _ringPhys = phys + kHeapRing;
  _marker = (volatile UInt32 *)(virt + kHeapMarker);
  _markerPhys = phys + kHeapMarker;
  copyWords(virt + kHeapTexCoords, sTexCoords, sizeof(sTexCoords));
  _texCoordsPhys = phys + kHeapTexCoords;
  copyWords(virt + kHeapQuad, sQuad, sizeof(sQuad));
  _quadPhys = phys + kHeapQuad;
  for (UInt32 i = 0; i < kRingBytes / 4; i++) {
    ((volatile UInt32 *)_ring)[i] = 0;
  }
  asm volatile("sync" ::: "memory");
  IOLog("XenosGPU: GPU heap: ring at 0x%08lx, marker 0x%08lx, vertices 0x%08lx/0x%08lx (uncached)\n",
        (unsigned long)_ringPhys, (unsigned long)_markerPhys, (unsigned long)_texCoordsPhys,
        (unsigned long)_quadPhys);
  return kIOReturnSuccess;
}

IOReturn XenosGPU::allocateBuffers(void) {
  _useHeap = (getProperty("XenosGPUTigerMemory") != kOSBooleanTrue);
  if (_useHeap) {
    return allocateHeapBuffers();
  }
  _texCoordsDesc = contiguousPage(sTexCoords, sizeof(sTexCoords), &_texCoordsPhys);
  _quadDesc = contiguousPage(sQuad, sizeof(sQuad), &_quadPhys);
  IOPhysicalAddress markerPhys = 0;
  _markerDesc = contiguousPage(NULL, 0, &markerPhys);
  if (_texCoordsDesc == NULL || _quadDesc == NULL || _markerDesc == NULL) {
    return kIOReturnNoMemory;
  }
  _marker = (volatile UInt32 *)_markerDesc->getBytesNoCopy();
  _markerPhys = markerPhys;

  // The CP needs the ring aligned to its size. Allocate twice the size and align the physical base.
  _ringDesc = IOBufferMemoryDescriptor::withOptions(kIOMemoryPhysicallyContiguous,
                                                    2 * kRingBytes - PAGE_SIZE, PAGE_SIZE);
  if (_ringDesc == NULL) {
    return kIOReturnNoMemory;
  }
  IOByteCount segment = 0;
  IOPhysicalAddress base = _ringDesc->getPhysicalSegment(0, &segment);
  UInt32 offset = (UInt32)(-base & (kRingBytes - 1));
  _ringPhys = base + offset;
  _ring = (volatile UInt8 *)_ringDesc->getBytesNoCopy() + offset;
  for (UInt32 page = 0; page < kRingBytes; page += PAGE_SIZE) {
    IOByteCount length = 0;
    if (_ringDesc->getPhysicalSegment(offset + page, &length) != _ringPhys + page || length < PAGE_SIZE) {
      IOLog("XenosGPU: ring not contiguous at page %lu\n", (unsigned long)(page / PAGE_SIZE));
      return kIOReturnNoResources;
    }
  }
  if (_ringPhys + kRingBytes > kDisplayMemoryStart) {
    return kIOReturnNoResources;
  }
  bzero((void *)_ring, kRingBytes);
  flushRange(_ring, kRingBytes);
  IOLog("XenosGPU: Tiger memory: ring at 0x%08lx (%lu bytes)\n", (unsigned long)_ringPhys,
        (unsigned long)kRingBytes);
  return kIOReturnSuccess;
}

void XenosGPU::releaseBuffers(void) {
  if (_heapMap != NULL) {
    _heapMap->release();
    _heapMap = NULL;
  }
  _marker = NULL;
  if (_ringDesc != NULL) {
    _ringDesc->release();
    _ringDesc = NULL;
  }
  if (_markerDesc != NULL) {
    _markerDesc->release();
    _markerDesc = NULL;
  }
  if (_texCoordsDesc != NULL) {
    _texCoordsDesc->release();
    _texCoordsDesc = NULL;
  }
  if (_quadDesc != NULL) {
    _quadDesc->release();
    _quadDesc = NULL;
  }
  _ring = NULL;
}

IOReturn XenosGPU::loadFirmware(void) {
  writeReg(kXenosRegCpPfpFirmwareAddr, 0);
  IODelay(100);
  for (UInt32 i = 0; i < ARRSIZE(sXenosFirmware0); i++) {
    writeReg(kXenosRegCpPfpFirmwareData, sXenosFirmware0[i]);
  }
  writeReg(kXenosRegCpPfpFirmwareAddr, 0);
  IODelay(100);
  for (UInt32 i = 0; i < ARRSIZE(sXenosFirmware0); i++) {
    readReg(kXenosRegCpPfpFirmwareData);
  }

  writeReg(kXenosRegCpMeRamWriteAddr, 0);
  for (UInt32 i = 0; i < ARRSIZE(sXenosFirmware1); i++) {
    writeReg(kXenosRegCpMeRamData, sXenosFirmware1[i]);
  }
  writeReg(kXenosRegCpMeRamReadAddr, 0);
  for (UInt32 i = 0; i < ARRSIZE(sXenosFirmware1); i++) {
    if (readReg(kXenosRegCpMeRamData) != sXenosFirmware1[i]) {
      IOLog("XenosGPU: ME firmware read-back mismatch at %lu\n", (unsigned long)i);
      return kIOReturnIOError;
    }
  }
  return kIOReturnSuccess;
}

void XenosGPU::haltCommandProcessor(void) {
  writeReg(kXenosRegCpMeControl, kCpMeHalt | kCpMeRunControl);
  (void)readReg(kXenosRegCpMeControl);
  IODelay(2000);
}

bool XenosGPU::ringDrained(void) {
  return readReg(kXenosRegCpRbReadPtr) == _ringWrite && readReg(kXenosRegCpRbWritePtr) == _ringWrite;
}

bool XenosGPU::waitForRingDrain(void) {
  for (UInt32 i = 0; i < kDrainPolls; i++) {
    if (ringDrained()) {
      return true;
    }
    IODelay(10);
  }
  return false;
}

void XenosGPU::submit(void) {
  if (!_useHeap) {
    flushRange(_ring, kRingBytes);
  }
  asm volatile("sync" ::: "memory");
  writeReg(kXenosRegCpRbWritePtr, _ringWrite);
}

bool XenosGPU::runMarker(const char *phase) {
  if (!ringDrained()) {
    return false;
  }
  IOPhysicalAddress phys = _markerPhys;
  volatile UInt32 *slot = _marker;
  *slot = kMarkerInitial;
  if (!_useHeap) {
    flushLine(slot);
  }
  ringPacket3(CP_MEM_WRITE, 2);
  ring((UInt32)phys | kMarkerSwap8In32);
  ring(kMarkerExpected);
  submit();

  UInt32 seen = kMarkerInitial, polls;
  for (polls = 0; polls < kMarkerPolls; polls++) {
    if (!_useHeap) {
      flushLine(slot);
    }
    seen = *slot;
    if (seen != kMarkerInitial) {
      break;
    }
    IODelay(10);
  }
  bool passed = (seen == kMarkerExpected) && waitForRingDrain();
  IOLog("XenosGPU: marker %s %s (0x%08lx after %lu polls)\n", phase, passed ? "PASS" : "FAIL",
        (unsigned long)seen, (unsigned long)polls);
  return passed;
}

void XenosGPU::initMicroEngine(void) {
  ring(0xC0114800);   // PM4_ME_INIT
  ring(0x000003FF);   // all fields present
  ring(0x00000000);
  ring(0x00000000);
  ring(0x2000 - 0x2000);  // RB_SURFACE_INFO
  ring(0x2080 - 0x2000);  // PA_SC_WINDOW_OFFSET
  ring(0x2100 - 0x2000);  // VGT_MAX_VTX_INDX
  ring(0x2180 - 0x2000);  // SQ_PROGRAM_CNTL
  ring(0x2200 - 0x2000);  // RB_DEPTHCONTROL
  ring(0x2280 - 0x2000);  // PA_SU_POINT_SIZE
  ring(0x2300 - 0x2000);  // PA_SC_LINE_CNTL
  ring(0x2380 - 0x2000);  // PA_SU_POLY_OFFSET_FRONT_SCALE
  ring(0x80000180);   // vertex and pixel shader start addresses
  ring(0x00000007);   // maximum contexts
  ring(0x00000000);
  ring(0x00000000);
  ring(0x00000000);   // protected mode error checking off
  ring(0x00000000);
  ring(0x00000000);
}

//
// Display state: draw the linear framebuffer as a texture over the whole EDRAM surface, then
// resolve (convert and tile) into the scanout buffer. Same state as XenosController::initFramebuffer.
//
void XenosGPU::initDisplayState(void) {
  ringPacket3(CP_INVALIDATE_STATE, 1);
  ring(0x00007FFF);
  ringPacket0(REG_A2XX_SQ_INST_STORE_MANAGMENT, 1);
  ring(0x00000180);
  ringConstant(REG_A2XX_SQ_VS_CONST, 0x00100020);
  ringConstant(REG_A2XX_SQ_PS_CONST, 0x000E0120);
  ringPacket3(CP_SET_CONSTANT, 3);
  ring(XENOS_CP_REG(REG_A2XX_VGT_MAX_VTX_INDX));
  ring(0xFFFFFFFF);
  ring(0x00000000);
  ringConstant(REG_A2XX_VGT_INDX_OFFSET, 0);
  ringConstant(REG_A2XX_VGT_VERTEX_REUSE_BLOCK_CNTL, 0x0000003B);
  ringConstant(REG_A2XX_SQ_CONTEXT_MISC, A2XX_SQ_CONTEXT_MISC_SC_SAMPLE_CNTL(CENTERS_ONLY));
  ringConstant(REG_A2XX_SQ_INTERPOLATOR_CNTL, 0xFFFFFFFF);
  ringConstant(REG_A2XX_PA_SC_AA_CONFIG, 0);
  ringConstant(REG_A2XX_PA_SC_AA_MASK, 0x0000FFFF);
  ringConstant(REG_A2XX_PA_SC_WINDOW_OFFSET, 0);

  float halfWidth = _width * 0.5f, halfHeight = _height * 0.5f;
  ringPacket3(CP_SET_CONSTANT, 5);
  ring(XENOS_CP_REG(REG_A2XX_PA_CL_VPORT_XSCALE));
  ring(fui(halfWidth));
  ring(fui(halfWidth));
  ring(fui(-halfHeight));
  ring(fui(halfHeight));

  ringConstant(REG_A2XX_PA_CL_CLIP_CNTL, 0);
  ringConstant(REG_A2XX_PA_CL_VTE_CNTL, A2XX_PA_CL_VTE_CNTL_VTX_W0_FMT |
               A2XX_PA_CL_VTE_CNTL_VPORT_X_SCALE_ENA | A2XX_PA_CL_VTE_CNTL_VPORT_X_OFFSET_ENA |
               A2XX_PA_CL_VTE_CNTL_VPORT_Y_SCALE_ENA | A2XX_PA_CL_VTE_CNTL_VPORT_Y_OFFSET_ENA);
  ringPacket3(CP_SET_CONSTANT, 5);
  ring(XENOS_CP_REG(REG_A2XX_PA_CL_GB_VERT_CLIP_ADJ));
  ring(fui(1.0f));
  ring(fui(1.0f));
  ring(fui(1.0f));
  ring(fui(1.0f));
  ringConstant(REG_A2XX_PA_SU_VTX_CNTL, A2XX_PA_SU_VTX_CNTL_PIX_CENTER(PIXCENTER_OGL));
  ringConstant(REG_A2XX_PA_SU_SC_MODE_CNTL, A2XX_PA_SU_SC_MODE_CNTL_PROVOKING_VTX_LAST |
               A2XX_PA_SU_SC_MODE_CNTL_FRONT_PTYPE(PC_DRAW_TRIANGLES) |
               A2XX_PA_SU_SC_MODE_CNTL_BACK_PTYPE(PC_DRAW_TRIANGLES));

  ringPacket3(CP_SET_CONSTANT, 3);
  ring(XENOS_CP_REG(REG_A2XX_PA_SC_WINDOW_SCISSOR_TL));
  ring(xy32(0, 0));
  ring(xy32(_width, _height));
  ringPacket3(CP_SET_CONSTANT, 3);
  ring(XENOS_CP_REG(REG_A2XX_PA_SC_SCREEN_SCISSOR_TL));
  ring(xy32(0, 0));
  ring(xy32(_width, _height));

  ringConstant(REG_A2XX_RB_BLEND_CONTROL, A2XX_RB_BLEND_CONTROL_COLOR_SRCBLEND(FACTOR_ONE) |
               A2XX_RB_BLEND_CONTROL_ALPHA_SRCBLEND(FACTOR_ONE) |
               A2XX_RB_BLEND_CONTROL_COLOR_DESTBLEND(FACTOR_ZERO) |
               A2XX_RB_BLEND_CONTROL_ALPHA_DESTBLEND(FACTOR_ZERO));
  ringConstant(REG_A2XX_RB_COLORCONTROL, A2XX_RB_COLORCONTROL_BLEND_DISABLE);
  ringConstant(REG_A2XX_RB_COPY_DEST_INFO, 0x1000A80);
  ringConstant(REG_A2XX_RB_ALPHA_REF, 0);

  ringPacket3(CP_WAIT_REG_EQ, 4);
  ring(0x000005D0);   // RBBM_STATUS
  ring(0x00000000);
  ring(0x5F601000);
  ring(0x00000001);

  ringPacket3(CP_INVALIDATE_STATE, 1);
  ring(0x00000300);
  ringConstant(REG_A2XX_RB_COLOR_MASK, A2XX_RB_COLOR_MASK_WRITE_RED | A2XX_RB_COLOR_MASK_WRITE_GREEN |
               A2XX_RB_COLOR_MASK_WRITE_BLUE | A2XX_RB_COLOR_MASK_WRITE_ALPHA);
  ringConstant(REG_A2XX_RB_DEPTHCONTROL, 0);
  ringPacket3(CP_SET_CONSTANT, 3);
  ring(XENOS_CP_REG(REG_A2XX_RB_STENCILREFMASK_BF));
  ring(0);
  ring(0);
  ringPacket3(CP_SET_CONSTANT, 4);
  ring(XENOS_CP_REG(REG_A2XX_RB_SURFACE_INFO));
  ring(ALIGN(_width, 32));
  ring(k_8_8_8_8 << 16);
  ring(A2XX_RB_DEPTH_INFO_DEPTH_BASE(ALIGN(_width, 32) * ALIGN(_height, 32)));

  // Vertex fetch: position and UVs (the SQ latches the three fetch pairs as one six-dword group).
  ringPacket3(CP_SET_CONSTANT, 7);
  ring((1 << 16) | 0x78);
  ring(_quadPhys | 3);
  ring(sizeof(sQuad) | 2);
  ring(_texCoordsPhys | 3);
  ring(sizeof(sTexCoords) | 2);
  ring(0);
  ring(0);

  // Texture 0: the linear framebuffer, bilinear.
  ringPacket3(CP_SET_CONSTANT, 7);
  ring(0x00010000);
  ring(A2XX_SQ_TEX_0_PITCH(ALIGN(_width, 32)) | A2XX_SQ_TEX_0_CLAMP_X(SQ_TEX_WRAP) |
       A2XX_SQ_TEX_0_CLAMP_Y(SQ_TEX_WRAP));
  ring(_linearPhys | FMT_8_8_8_8);
  ring(A2XX_SQ_TEX_2_HEIGHT(_height - 1) | A2XX_SQ_TEX_2_WIDTH(_width - 1));
  ring(A2XX_SQ_TEX_3_SWIZ_X(SQ_TEX_Y) | A2XX_SQ_TEX_3_SWIZ_Y(SQ_TEX_Z) | A2XX_SQ_TEX_3_SWIZ_Z(SQ_TEX_W) |
       A2XX_SQ_TEX_3_SWIZ_W(SQ_TEX_X) | A2XX_SQ_TEX_3_XY_MAG_FILTER(SQ_TEX_FILTER_BILINEAR) |
       A2XX_SQ_TEX_3_XY_MIN_FILTER(SQ_TEX_FILTER_BILINEAR));
  ring(0);
  ring(0x00000200);

  // Resolve target: the tiled scanout buffer, with format conversion.
  ringPacket3(CP_SET_CONSTANT, 5);
  ring(XENOS_CP_REG(REG_A2XX_RB_COPY_CONTROL));
  ring(1 << 20);
  ring(_scanoutPhys);
  ring(xy32(_width, _height));
  ring((COLORX_S8_8_8_8 << 7) | (1 << 24));

  // Vertex shader and texture pixel shader (XenosController::setGpuDisplayShaders).
  ringPacket3(CP_IM_LOAD_IMMEDIATE, 2 + 21);
  ring(0x00000000);
  ring(0x00000015);
  ring(0x000D2003); ring(0x00001000); ring(0xC2000000);
  ring(0x00001005); ring(0x00001000); ring(0xC4000000);
  ring(0x00001006); ring(0x00002000); ring(0x00000000);
  ring(0x0B482000); ring(0x00253B48); ring(0x00000002);
  ring(0x01481000); ring(0x40393A88); ring(0x00000003);
  ring(0x140F803E); ring(0x00000000); ring(0xE2010100);
  ring(0x140F8000); ring(0x00000000); ring(0xE2020200);
  ringConstant(REG_A2XX_SQ_PROGRAM_CNTL, 0x10030002);
  ringPacket3(CP_IM_LOAD_IMMEDIATE, 2 + 12);
  ring(0x00000001);
  ring(0x0000000C);
  ring(0x00031002); ring(0x00001000); ring(0xC4000000);
  ring(0x00001003); ring(0x00002000); ring(0x00000000);
  ring(0x10000001); ring(0x1FFFF688); ring(0x00000000);
  ring(0x140F8000); ring(0x00000000); ring(0xE2000000);
}

//
// Resets the 3D blocks the way libxenon's Xe_pReset does (drivers/xenos/xe.c): write 0x8064, then
// 0x11800, to RBBM_SOFT_RESET, each followed by zero, until RBBM_STATUS (0x1740) shows GUI_ACTIVE
// clear. These values leave out bit 0 (CP) and bits 3 and 4. Writing all ones hangs the whole console
// (2026-11-25). Bounded to 100 rounds. Returns false if the pipeline stays busy.
//
//
// Configuration registers the 3D block reset may clear. libxenon's Xe_pInit1 rewrites some of them
// after its reset, but with values that differ from XeLL's (0x0E6C, 0x340C). So the driver takes a
// snapshot before the reset and writes back whatever the reset changed. CP registers are left out
// (startCommandProcessor programs them anyway), as are status, performance and fault registers.
//
static const UInt16 kConfigRegisters[] = {
  0x00EC, 0x00F4, 0x01A8, 0x0E6C,                 // RBBM_CNTL, 0x00F4, 0x01A8, RBBM_DEBUG
  0x0100, 0x0104, 0x0108, 0x0118, 0x011C,         // MH_MMU_CONFIG, VA_RANGE, PT_BASE, MPU_BASE/END
  0x2900, 0x2908,                                 // MH_ARBITER_CONFIG, MH_INTERRUPT_MASK
  0x2054, 0x2154, 0x3214, 0x3294, 0x3300, 0x3400, 0x3408, 0x340C,
  0x3C10, 0x3C40, 0x87E4, 0x6110
};

bool XenosGPU::resetGraphicsBlocks(const char *when) {
  UInt32 before = readReg(kXenosRegRbbmStatus);
  const UInt32 count = sizeof(kConfigRegisters) / sizeof(kConfigRegisters[0]);
  UInt32 snapshot[sizeof(kConfigRegisters) / sizeof(kConfigRegisters[0])];
  for (UInt32 i = 0; i < count; i++) {
    snapshot[i] = readReg(kConfigRegisters[i]);
  }
  // Only a busy pipeline is reset, unless the personality key XenosGPUForceReset is set. A forced
  // reset on a fresh GPU changed none of the configuration registers and still stalled the GPU at
  // frame 74 (2026-11-26): the reset leaves a state no readable register shows.
  if (!_forceReset && !(before & 0x80000000)) {
    IOLog("XenosGPU: 3D block reset %s skipped, pipeline idle (RBBM_STATUS 0x%08lx)\n", when,
          (unsigned long)before);
    return true;
  }
  UInt32 rounds;
  for (rounds = 0; rounds < 100; rounds++) {
    if (rounds > 0 && !(readReg(kXenosRegRbbmStatus) & 0x80000000)) {
      break;
    }
    writeReg(kXenosRegRbbmSoftReset, 0x8064);
    (void)readReg(kXenosRegRbbmSoftReset);
    writeReg(kXenosRegRbbmSoftReset, 0);
    writeReg(kXenosRegRbbmSoftReset, 0x11800);
    (void)readReg(kXenosRegRbbmSoftReset);
    writeReg(kXenosRegRbbmSoftReset, 0);
    IODelay(1000);
  }
  UInt32 after = readReg(kXenosRegRbbmStatus);
  UInt32 config = readReg(kXenosRegConfigControl);
  IOLog("XenosGPU: 3D block reset %s: RBBM_STATUS 0x%08lx -> 0x%08lx after %lu rounds, 0x00E0 = 0x%08lx\n",
        when, (unsigned long)before, (unsigned long)after, (unsigned long)rounds, (unsigned long)config);
  UInt32 restored = 0;
  for (UInt32 i = 0; i < count; i++) {
    UInt32 now = readReg(kConfigRegisters[i]);
    if (now != snapshot[i]) {
      writeReg(kConfigRegisters[i], snapshot[i]);
      IOLog("XenosGPU: reset changed 0x%04x: 0x%08lx -> 0x%08lx, restored (now 0x%08lx)\n",
            (unsigned)kConfigRegisters[i], (unsigned long)snapshot[i], (unsigned long)now,
            (unsigned long)readReg(kConfigRegisters[i]));
      restored++;
    }
  }
  IOLog("XenosGPU: 3D block reset %s: %lu of %lu configuration registers restored\n", when,
        (unsigned long)restored, (unsigned long)count);
  return !(after & 0x80000000);
}

//
// Registers libxenon's Xe_pSetup, Xe_pResetCP and Xe_pMasterInit (drivers/xenos/xe.c) write around the
// CP start, and which XenonCoreGraphics never wrote. XeLL runs them at every boot, so a first session
// starts with XeLL's values; if the CP reset clears some of them, a second session does not, and it
// stalled at frame 71 (2026-11-25). Tested 2026-11-25: they do not fix that, and XeLL's own values
// differ (0x3214 = 3, 0x3408 = 0x10800, 0x2054/0x2154 = 0xFF00, 0x3C10 = 0, 0x3C40 = 0x1A), so they
// are off unless the personality key XenosGPULibxenonInit is set.
//
static const UInt32 kEdramPolls = 1000;

void XenosGPU::libxenonCPRegisters(void) {
  // Xe_pSetup / Xe_pResetCP, right after the ME runs.
  IOLog("XenosGPU: libxenon CP registers before: 0x3214=0x%08lx 0x3294=0x%08lx 0x3408=0x%08lx\n",
        (unsigned long)readReg(0x3214), (unsigned long)readReg(0x3294), (unsigned long)readReg(0x3408));
  writeReg(0x3214, 7);
  writeReg(0x3294, 1);
  writeReg(0x3408, 0x800);
}

bool XenosGPU::waitEdramIdle(UInt32 *value) {
  for (UInt32 i = 0; i < kEdramPolls; i++) {
    *value = readReg(0x3C4C);
    if (!(*value & 0x80000000)) {
      return true;
    }
    IODelay(10);
  }
  return false;
}

void XenosGPU::libxenonMasterInit(void) {
  IOLog("XenosGPU: libxenon master init before: 0x07D4=0x%08lx 0x2054=0x%08lx 0x2154=0x%08lx "
        "0x3C10=0x%08lx 0x3C40=0x%08lx 0x3C4C=0x%08lx 0x87E4=0x%08lx\n",
        (unsigned long)readReg(0x07D4), (unsigned long)readReg(0x2054), (unsigned long)readReg(0x2154),
        (unsigned long)readReg(0x3C10), (unsigned long)readReg(0x3C40), (unsigned long)readReg(0x3C4C),
        (unsigned long)readReg(0x87E4));
  writeReg(0x07D4, 0);
  writeReg(0x07D4, 1);
  writeReg(0x2054, 0x1E);
  writeReg(0x2154, 0x1E);
  writeReg(0x3C10, 0xD);
  writeReg(0x3C40, 0x17);
  writeReg(0x3C48, 0);
  UInt32 first = 0, second = 0;
  bool firstIdle = waitEdramIdle(&first);
  writeReg(0x3C40, 0x1017);
  writeReg(0x3C48, 0);
  bool secondIdle = waitEdramIdle(&second);
  writeReg(0x87E4, 0x17);
  IOLog("XenosGPU: libxenon master init done: EDRAM waits %s (0x%08lx) and %s (0x%08lx)\n",
        firstIdle ? "ok" : "TIMEOUT", (unsigned long)first, secondIdle ? "ok" : "TIMEOUT",
        (unsigned long)second);
}

//
// Register dump for comparing the GPU state a session starts from (2026-11-25: a first session after
// boot runs, every later one stalls at frame 71). Only registers named in the a2xx headers, the ones
// XeLL and libxenon set, and five EDRAM registers libxenon uses; no data ports or write-only
// registers (PFP/ME RAM data, state debug data, INT_ACK, MMU invalidate).
//
static const UInt16 kDumpRegisters[] = {
  0x0004, 0x00E0, 0x00E4, 0x00E8, 0x00EC, 0x00F0, 0x00F4, 0x0100, 0x0104, 0x0108,
  0x010C, 0x0110, 0x0118, 0x011C, 0x01A8, 0x0300, 0x0700, 0x0704, 0x070C, 0x0710,
  0x0714, 0x0718, 0x0720, 0x0754, 0x0758, 0x075C, 0x0760, 0x0764, 0x0770, 0x0774,
  0x07B0, 0x07C8, 0x07CC, 0x07D8, 0x07DC, 0x07F0, 0x07F4, 0x07F8, 0x07FC, 0x0E50,
  0x0E6C, 0x0E70, 0x0E74, 0x0E84, 0x0ECC, 0x0ED0, 0x0ED4, 0x0EDC, 0x1100, 0x110C,
  0x1110, 0x1134, 0x1138, 0x113C, 0x1148, 0x1160, 0x1164, 0x1168, 0x116C, 0x11FC,
  0x15E0, 0x15E4, 0x15E8, 0x15EC, 0x15F0, 0x15F4, 0x15F8, 0x15FC, 0x1740, 0x1800,
  0x1804, 0x1808, 0x180C, 0x1810, 0x1814, 0x1818, 0x181C, 0x1820, 0x1824, 0x1828,
  0x182C, 0x1830, 0x1834, 0x1838, 0x1848, 0x184C, 0x1850, 0x2054, 0x2154, 0x28A4,
  0x28A8, 0x28AC, 0x2900, 0x2908, 0x3214, 0x3294, 0x3300, 0x3400, 0x3408, 0x340C,
  0x3C08, 0x3C10, 0x3C40, 0x3C44, 0x3C48, 0x3C4C, 0x6110, 0x8018, 0x87E4
};

void XenosGPU::dumpRegisters(const char *when) {
  const UInt32 count = sizeof(kDumpRegisters) / sizeof(kDumpRegisters[0]);
  for (UInt32 i = 0; i < count; i += 8) {
    char line[256];
    UInt32 used = 0;
    for (UInt32 j = i; j < count && j < i + 8 && used < sizeof(line) - 24; j++) {
      used += snprintf(line + used, sizeof(line) - used, " %04x=%08lx", (unsigned)kDumpRegisters[j],
                       (unsigned long)readReg(kDumpRegisters[j]));
    }
    IOLog("XenosGPU: regs %s%s\n", when, line);
  }
}

//
// Before halting the CP at unload: a drained ring only means the CP has read every packet; the last
// resolve can still be running. Halting the CP then left the GPU busy (RBBM_STATUS 0x80050110 at the
// next load) or made the next session stall at frame 71-73 (2026-11-25). Wait for idle on the GPU
// (CP_WAIT_FOR_IDLE, then a marker), then for GUI_ACTIVE to clear (at most 500 ms).
//
bool XenosGPU::quiesce(void) {
  ringPacket3(CP_WAIT_FOR_IDLE, 1);
  ring(0);
  submit();
  bool marker = waitForRingDrain() && runMarker("stop");
  UInt32 status = 0, polls;
  for (polls = 0; polls < 50000; polls++) {
    status = readReg(kXenosRegRbbmStatus);
    if (!(status & 0x80000000)) {
      break;
    }
    IODelay(10);
  }
  IOLog("XenosGPU: quiesce: marker %s, RBBM_STATUS 0x%08lx after %lu polls\n", marker ? "ok" : "FAIL",
        (unsigned long)status, (unsigned long)polls);
  return marker && !(status & 0x80000000);
}

const char *XenosGPU::startCommandProcessor(void) {
  haltCommandProcessor();
  if (!resetGraphicsBlocks("at start")) {
    return "3d-reset";
  }
  writeReg(kXenosRegRbbmSoftReset, 1);
  (void)readReg(kXenosRegRbbmSoftReset);
  IODelay(1000);
  writeReg(kXenosRegRbbmSoftReset, 0);
  (void)readReg(kXenosRegRbbmSoftReset);
  IODelay(1000);
  haltCommandProcessor();
  if (!(readReg(kXenosRegCpMeControl) & kCpMeHalt)) {
    return "halt";
  }

  _ringWrite = 0;
  writeReg(kXenosRegCpRbControl, kCpRingControl | kCpAllowRptrWrite);
  writeReg(kXenosRegCpRbReadPtrWrite, 0);
  writeReg(kXenosRegCpRbWritePtr, 0);
  writeReg(kXenosRegCpRbControl, kCpRingControl);
  writeReg(kXenosRegCpRbBase, _ringPhys);
  writeReg(kXenosRegCpRbWritePtrDelay, 0x10);
  writeReg(kXenosRegScratchUMask, 0);
  writeReg(kXenosRegScratchAddr, 0);
  writeReg(kXenosRegCpIntControl, 0);     // No GPU interrupts (rule until vector 0x58 is understood).
  writeReg(kXenosRegCpIntAck, 0xFFFF);
  writeReg(kXenosRegCpDebug, 0);
  writeReg(kXenosRegCpSnoopControl, 0x1A);
  if (!ringDrained()) {
    return "reset-pointers";
  }
  if (loadFirmware() != kIOReturnSuccess) {
    return "firmware";
  }
  writeReg(kXenosRegCpMeControl, kCpMeRunControl);
  if (readReg(kXenosRegCpMeControl) & kCpMeHalt) {
    return "start";
  }
  IODelay(1000);
  // Pixel shader event write-back. XeLL leaves CP_ME_PS_EVENT_ADDR/DATA (0x1818/0x181C) set
  // (0x01B7C002 / 0x6D); after a XenosGPU session they read 0, and every following session stalled at
  // frame 71-73 (register comparison, 2026-11-25). Tested: the write does not stick (0 again at unload)
  // and the next start found the GPU busy, so it is off unless XenosGPUPsEvent is set.
  if (_psEvent) {
    writeReg(0x1818, (_markerPhys + 0x100) | kMarkerSwap8In32);
    writeReg(0x181C, 0x50534556);   // "PSEV"
  }
  if (_libxenonInit) {
    libxenonCPRegisters();
    libxenonMasterInit();
  }

  initMicroEngine();
  submit();
  if (!waitForRingDrain()) {
    return "me-init";
  }
  if (!runMarker("command-processor")) {
    return "command-processor-marker";
  }
  // Page-crossing check: both stalls came just past ring dword 0x400 (4 KiB). Pad with type-2 NOP
  // packets to dword 0x420, then check a marker there, before any drawing.
  if (_pageCheck) {
    while (_ringWrite < 0x420) {
      ring(0x80000000);
    }
    submit();
    if (!waitForRingDrain() || !runMarker("page-cross")) {
      return "page-cross-marker";
    }
  }
  initDisplayState();
  submit();
  if (!waitForRingDrain()) {
    return "display-state";
  }
  if (!runMarker("display-state")) {
    return "display-state-marker";
  }
  return NULL;
}

//
// Once per boot. After a XenosGPU session (or the libxenon 3D reset) the GPU's 3D pipeline stalls
// after about 72 resolves in any later session, and only a reboot (XeLL's initialization) clears it
// (2026-11-25/26, docs/gpu-driver-plan.md, step 1.1). So the first start in a boot leaves a mark in
// the GPU heap (magic and time base); a later load in the same boot sees it and keeps the CPU refresh
// instead of stalling the GPU. A power-off clears the RAM, and after a restart the time base starts
// again near zero, so the next boot starts normally. XenosGPUAllowRestart overrides this.
//
static const UInt32 kHeapBootMark = 0xF000;
static const UInt32 kBootMarkMagic = 0x58475055;  // "XGPU"

bool XenosGPU::claimBoot(void) {
  if (!_useHeap || _heapMap == NULL) {
    return true;   // Tiger memory (XenosGPUTigerMemory): no mark.
  }
  volatile UInt32 *mark = (volatile UInt32 *)((volatile UInt8 *)_heapMap->getVirtualAddress() + kHeapBootMark);
  UInt32 tbu, tbl, tbu2;
  do {
    __asm__ volatile ("mftbu %0" : "=r" (tbu));
    __asm__ volatile ("mftb %0" : "=r" (tbl));
    __asm__ volatile ("mftbu %0" : "=r" (tbu2));
  } while (tbu != tbu2);
  bool earlier = (mark[0] == kBootMarkMagic) &&
                 (mark[1] < tbu || (mark[1] == tbu && mark[2] < tbl));
  if (earlier && getProperty("XenosGPUAllowRestart") != kOSBooleanTrue) {
    IOLog("XenosGPU: the GPU was already started in this boot (time base 0x%08lx%08lx, now 0x%08lx%08lx); "
          "a second start stalls it, so the CPU refresh keeps the display until the next boot\n",
          (unsigned long)mark[1], (unsigned long)mark[2], (unsigned long)tbu, (unsigned long)tbl);
    return false;
  }
  if (earlier) {
    IOLog("XenosGPU: second start in this boot allowed by XenosGPUAllowRestart\n");
  }
  mark[1] = tbu;
  mark[2] = tbl;
  asm volatile("sync" ::: "memory");
  mark[0] = kBootMarkMagic;
  asm volatile("sync" ::: "memory");
  return true;
}

IOReturn XenosGPU::startGPU(void) {
  IOReturn status = allocateBuffers();
  if (status != kIOReturnSuccess) {
    IOLog("XenosGPU: buffers: 0x%08lx\n", (unsigned long)status);
    return status;
  }
  if (!claimBoot()) {
    return kIOReturnBusy;
  }
  for (UInt32 attempt = 1; attempt <= kStartupAttempts; attempt++) {
    const char *failure = startCommandProcessor();
    if (failure == NULL) {
      IOLog("XenosGPU: command processor PASS attempt %lu rptr=0x%04lx wptr=0x%04lx\n",
            (unsigned long)attempt, (unsigned long)readReg(kXenosRegCpRbReadPtr),
            (unsigned long)readReg(kXenosRegCpRbWritePtr));
      return kIOReturnSuccess;
    }
    IOLog("XenosGPU: command processor FAIL attempt %lu phase %s rptr=0x%04lx wptr=0x%04lx "
          "me=0x%08lx rbbm=0x%08lx\n", (unsigned long)attempt, failure,
          (unsigned long)readReg(kXenosRegCpRbReadPtr), (unsigned long)readReg(kXenosRegCpRbWritePtr),
          (unsigned long)readReg(kXenosRegCpMeStatus), (unsigned long)readReg(kXenosRegRbbmStatus));
  }
  haltCommandProcessor();
  return kIOReturnIOError;
}
