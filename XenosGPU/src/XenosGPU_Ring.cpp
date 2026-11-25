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

IOReturn XenosGPU::allocateBuffers(void) {
  _texCoordsDesc = contiguousPage(sTexCoords, sizeof(sTexCoords), &_texCoordsPhys);
  _quadDesc = contiguousPage(sQuad, sizeof(sQuad), &_quadPhys);
  IOPhysicalAddress markerPhys = 0;
  _markerDesc = contiguousPage(NULL, 0, &markerPhys);
  if (_texCoordsDesc == NULL || _quadDesc == NULL || _markerDesc == NULL) {
    return kIOReturnNoMemory;
  }

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
  IOLog("XenosGPU: ring at 0x%08lx (%lu bytes)\n", (unsigned long)_ringPhys, (unsigned long)kRingBytes);
  return kIOReturnSuccess;
}

void XenosGPU::releaseBuffers(void) {
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
  flushRange(_ring, kRingBytes);
  writeReg(kXenosRegCpRbWritePtr, _ringWrite);
}

bool XenosGPU::runMarker(const char *phase) {
  if (!ringDrained()) {
    return false;
  }
  IOByteCount length = 0;
  IOPhysicalAddress phys = _markerDesc->getPhysicalSegment(0, &length);
  volatile UInt32 *slot = (volatile UInt32 *)_markerDesc->getBytesNoCopy();
  *slot = kMarkerInitial;
  flushLine(slot);
  ringPacket3(CP_MEM_WRITE, 2);
  ring((UInt32)phys | kMarkerSwap8In32);
  ring(kMarkerExpected);
  submit();

  UInt32 seen = kMarkerInitial, polls;
  for (polls = 0; polls < kMarkerPolls; polls++) {
    flushLine(slot);
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

const char *XenosGPU::startCommandProcessor(void) {
  haltCommandProcessor();
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

  initMicroEngine();
  submit();
  if (!waitForRingDrain()) {
    return "me-init";
  }
  if (!runMarker("command-processor")) {
    return "command-processor-marker";
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

IOReturn XenosGPU::startGPU(void) {
  IOReturn status = allocateBuffers();
  if (status != kIOReturnSuccess) {
    IOLog("XenosGPU: buffers: 0x%08lx\n", (unsigned long)status);
    return status;
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
