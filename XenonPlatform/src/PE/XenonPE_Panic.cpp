//
//  XenonPE_Panic.cpp
//  Xbox 360 platform expert
//
//  Panic text on screen, the GPU heap and GPU interrupt silencing (GPU plan steps 0.4 and 0.5,
//  docs/gpu-panic-and-interrupts.md and docs/gpu-memory-map.md). Each part is off unless the XenonPE
//  personality enables it: XenonPanicScreen, XenonGPUHeap, XenonGPUInterrupt.
//
//  On a panic, Tiger's Debugger() packs the panic text (packAsc) and hands it to savePanicInfo(). The
//  default ends in NVRAM code that does nothing on this console. With XenonPanicScreen, the text is
//  drawn with the kernel's own console font (iso_font, looked up at runtime, no new import) into the
//  linear framebuffer and into the tiled scanout buffer, and copied to a record at 0x08100000 in RAM
//  Tiger does not manage. Returning the length makes the kernel stop the other CPUs and hang in
//  haltRestart(kPEHangCPU) without rebooting, so the text stays on screen for a photo.
//
//  Code that runs at panic time takes no locks, allocates nothing and does not log.
//

#include "XenonPE.hpp"
#include "XenosRegs.hpp"
#include <pexpert/ppc/boot.h>

// Unused RAM between OpenBIOS's hash table and Tiger's second DRAM bank (docs/gpu-memory-map.md).
// The first MiB holds the panic record; the GPU heap is the rest.
static const UInt32 kXenonUnusedRAMBase   = 0x08100000;
static const UInt32 kXenonUnusedRAMEnd    = 0x10000000;
static const UInt32 kXenonPanicRecordBase = kXenonUnusedRAMBase;
static const UInt32 kXenonPanicRecordSize = 0x4000;
static const UInt32 kXenonGPUHeapBase     = 0x08200000;
static const UInt32 kXenonGPUHeapEnd      = kXenonUnusedRAMEnd;

static const UInt32 kXenonPanicMagic      = 0x58504E43;  // "XPNC"
static const UInt32 kXenonPanicTextMax    = 4096;

// Xenos interrupt registers (dword index * 4): adreno_common.xml.h and a2xx.xml.h.
static const UInt32 kXenosRegCpIntCntl    = 0x1F2 * 4;
static const UInt32 kXenosRegRbbmIntCntl  = 0x3B4 * 4;
static const UInt32 kXenosRegRbbmIntAck   = 0x3B6 * 4;

// Console font in the kernel: 256 glyphs of 16 rows, one byte per row, bit 0 is the leftmost pixel.
static const UInt32 kIsoFontHeight        = 16;
static const UInt32 kIsoFontWidth         = 8;

static const UInt32 kPanicBackground      = 0xFF102040;
static const UInt32 kPanicForeground      = 0xFFFFFFFF;
static const UInt32 kPanicTitle           = 0xFFFFD040;

static char sXenonPanicText[kXenonPanicTextMax + 1];

struct XenonPanicRecord {
  UInt32  magic;
  UInt32  version;
  UInt32  packedLength;
  UInt32  textLength;
  UInt32  timebaseHigh;
  UInt32  timebaseLow;
  char    text[kXenonPanicRecordSize - 24];
};

//
// Reverses Tiger's packAsc: eight 7-bit characters per seven bytes, most significant bits first.
//
static UInt32 unpackPanicText(const UInt8 *packed, UInt32 length, char *text, UInt32 max) {
  UInt32 out = 0;
  for (UInt32 in = 0; in + 7 <= length && out + 8 <= max; in += 7) {
    // Character k is bits 7k..7k+6 of the 56-bit group, counted from the most significant bit.
    for (UInt32 k = 0; k < 8; k++) {
      UInt32 c = 0;
      for (UInt32 b = 0; b < 7; b++) {
        UInt32 bit = 7 * k + b;
        c = (c << 1) | ((packed[in + (bit >> 3)] >> (7 - (bit & 7))) & 1);
      }
      text[out++] = (char)c;
    }
  }
  // The last group may be padding.
  while (out > 0 && text[out - 1] == 0) {
    out--;
  }
  text[out] = 0;
  return out;
}

//
// Sets up the panic screen, the GPU heap and the panic record (called from start()).
//
void XenonPE::setupPanicAndHeap(void) {
  _panicScreen = false;
  _isoFont = NULL;
  _panicRecordMap = NULL;
  _gpuInterruptRouting = (getProperty("XenonGPUInterrupt") == kOSBooleanTrue);

  //
  // The unused RAM must lie outside every DRAM bank Tiger manages.
  //
  bool unusedRAMFree = true;
  const boot_args *args = (const boot_args *)PE_state.bootArgs;
  for (UInt32 i = 0; args != NULL && i < kMaxDRAMBanks; i++) {
    UInt32 base = args->PhysicalDRAM[i].base;
    UInt32 size = args->PhysicalDRAM[i].size;
    if (size != 0 && base < kXenonUnusedRAMEnd && base + size > kXenonUnusedRAMBase) {
      unusedRAMFree = false;
    }
  }
  if (args == NULL || !unusedRAMFree) {
    XESYSLOG("RAM at 0x%X-0x%X overlaps Tiger's memory, no GPU heap and no panic record",
             kXenonUnusedRAMBase, kXenonUnusedRAMEnd - 1);
    return;
  }

  if (getProperty("XenonGPUHeap") == kOSBooleanTrue) {
    setProperty("XenosGPUHeapBase", kXenonGPUHeapBase, 32);
    setProperty("XenosGPUHeapSize", kXenonGPUHeapEnd - kXenonGPUHeapBase, 32);
    XESYSLOG("GPU heap 0x%X-0x%X (%u MiB) published", kXenonGPUHeapBase, kXenonGPUHeapEnd - 1,
             (kXenonGPUHeapEnd - kXenonGPUHeapBase) >> 20);
  }

  if (getProperty("XenonPanicScreen") == kOSBooleanTrue) {
    _isoFont = (const UInt8 *)resolveKernelSymbolExact("_iso_font");
    IODeviceMemory *record = IODeviceMemory::withRange(kXenonPanicRecordBase, kXenonPanicRecordSize);
    if (record != NULL) {
      _panicRecordMap = record->map(kIOMapInhibitCache);
      record->release();
    }
    _panicScreen = true;
    XESYSLOG("Panic screen armed: font %p, record %s at 0x%X", _isoFont,
             _panicRecordMap != NULL ? "mapped" : "not mapped", kXenonPanicRecordBase);

    // Report a record left by an earlier panic in this RAM (it may not survive a restart).
    if (_panicRecordMap != NULL) {
      volatile XenonPanicRecord *last = (volatile XenonPanicRecord *)_panicRecordMap->getVirtualAddress();
      if (last->magic == kXenonPanicMagic && last->textLength < sizeof(last->text)) {
        XESYSLOG("Panic record from before this boot: %u characters, first line follows",
                 last->textLength);
        char line[128];
        UInt32 i;
        for (i = 0; i < sizeof(line) - 1 && i < last->textLength && last->text[i] != '\n'; i++) {
          line[i] = last->text[i];
        }
        line[i] = 0;
        XESYSLOG("%s", line);
      } else {
        XESYSLOG("No panic record from before this boot");
      }
    }
  }
}

//
// Writes one pixel to both framebuffers. Tiled index formula from refreshFramebuffer (libxenon).
//
inline void XenonPE::panicPixel(UInt32 x, UInt32 y, UInt32 width, UInt32 rowWords, UInt32 argb) {
  UInt32 index = (((y >> 5) * 32 * width + ((x >> 5) << 10) + (x & 3) + ((y & 1) << 2) +
                  (((x & 31) >> 2) << 3) + (((y & 31) >> 1) << 6)) ^ ((y & 8) << 2));
  ((volatile UInt32 *)_fbMem)[x + y * rowWords] = argb;
  ((volatile UInt32 *)_gpuMem)[index] = OSSwapHostToLittleInt32(argb);
}

//
// Draws a title and the text over the whole screen. Safe at panic time.
//
void XenonPE::drawPanicScreen(const char *title, const char *text) {
  PE_Video video;
  getConsoleInfo(&video);
  UInt32 width = video.v_width, height = video.v_height;
  UInt32 rowWords = video.v_rowBytes / sizeof(UInt32);
  if (width == 0 || height == 0 || width > 4096 || height > 4096 || _fbMem == NULL || _gpuMem == NULL) {
    return;
  }

  // Let a queued GPU resolve finish first, so it cannot overwrite the text. Bounded to about
  // 100 ms of time base (50 MHz); no IODelay, to keep XenonPlatform's kernel imports unchanged.
  UInt32 start, now;
  __asm__ volatile ("mftb %0" : "=r" (start));
  do {
    if (OSReadBigInt32(_gpuMmioMem, kXenosRegCpRbReadPtr) == OSReadBigInt32(_gpuMmioMem, kXenosRegCpRbWritePtr)) {
      break;
    }
    __asm__ volatile ("mftb %0" : "=r" (now));
  } while ((now - start) < 5000000);

  for (UInt32 y = 0; y < height; y++) {
    for (UInt32 x = 0; x < width; x++) {
      panicPixel(x, y, width, rowWords, kPanicBackground);
    }
  }
  if (_isoFont == NULL) {
    return;
  }

  // Scale 2 (120 x 33 characters at 1080p) when the text fits, else scale 1.
  UInt32 scale = 2;
  for (int pass = 0; pass < 2; pass++) {
    UInt32 columns = (width - 32) / (kIsoFontWidth * scale);
    UInt32 rows = (height - 32) / (kIsoFontHeight * scale);
    UInt32 needed = 2, column = 0;
    for (const char *p = text; *p != 0; p++) {
      if (*p == '\n' || column == columns) {
        needed++;
        column = 0;
      }
      if (*p != '\n') {
        column++;
      }
    }
    if (needed <= rows || scale == 1) {
      break;
    }
    scale = 1;
  }
  UInt32 columns = (width - 32) / (kIsoFontWidth * scale);
  UInt32 rows = (height - 32) / (kIsoFontHeight * scale);

  UInt32 column = 0, row = 0;
  for (int part = 0; part < 2; part++) {
    const char *p = (part == 0) ? title : text;
    UInt32 color = (part == 0) ? kPanicTitle : kPanicForeground;
    for (; p != NULL && *p != 0 && row < rows; p++) {
      if (*p == '\n' || column == columns) {
        row++;
        column = 0;
        if (*p == '\n') {
          continue;
        }
        if (row >= rows) {
          break;
        }
      }
      UInt8 c = (UInt8)*p;
      if (c == '\t') {
        c = ' ';
      }
      const UInt8 *glyph = _isoFont + c * kIsoFontHeight;
      UInt32 x0 = 16 + column * kIsoFontWidth * scale;
      UInt32 y0 = 16 + row * kIsoFontHeight * scale;
      for (UInt32 gy = 0; gy < kIsoFontHeight; gy++) {
        UInt8 bits = glyph[gy];
        for (UInt32 gx = 0; gx < kIsoFontWidth; gx++) {
          if ((bits & (1 << gx)) == 0) {
            continue;
          }
          for (UInt32 sy = 0; sy < scale; sy++) {
            for (UInt32 sx = 0; sx < scale; sx++) {
              panicPixel(x0 + gx * scale + sx, y0 + gy * scale + sy, width, rowWords, color);
            }
          }
        }
      }
      column++;
    }
    row += (part == 0) ? 2 : 0;
    column = 0;
  }
}

//
// Overrides IODTPlatformExpert::savePanicInfo(). Runs at panic time.
//
IOByteCount XenonPE::savePanicInfo(UInt8 *buffer, IOByteCount length) {
  if (!_panicScreen || buffer == NULL || length == 0) {
    return super::savePanicInfo(buffer, length);
  }

  UInt32 textLength = unpackPanicText(buffer, length, sXenonPanicText, kXenonPanicTextMax);

  if (_panicRecordMap != NULL) {
    volatile XenonPanicRecord *record = (volatile XenonPanicRecord *)_panicRecordMap->getVirtualAddress();
    UInt32 tbu, tbl;
    __asm__ volatile ("mftbu %0" : "=r" (tbu));
    __asm__ volatile ("mftb %0" : "=r" (tbl));
    UInt32 n = textLength < sizeof(record->text) - 1 ? textLength : sizeof(record->text) - 1;
    for (UInt32 i = 0; i < n; i++) {
      record->text[i] = sXenonPanicText[i];
    }
    record->text[n] = 0;
    record->version = 1;
    record->packedLength = length;
    record->textLength = n;
    record->timebaseHigh = tbu;
    record->timebaseLow = tbl;
    __asm__ volatile ("sync");
    record->magic = kXenonPanicMagic;
    __asm__ volatile ("sync");
  }

  drawPanicScreen("Mac OS X panic on Xbox 360 (XenonPE panic screen). Please photograph this screen.",
                  sXenonPanicText);

  // Non-zero: the kernel stops the other CPUs and hangs without rebooting.
  return length;
}

//
// Test of the panic screen without a panic, through callPlatformFunction: "XenonPanicScreenTest" stops
// the CPU framebuffer refresh and draws a sample, "XenonPanicScreenTestEnd" starts the refresh again.
// The caller waits in between (no IOSleep here, to keep XenonPlatform's kernel imports unchanged).
//
IOReturn XenonPE::testPanicScreen(bool begin) {
  if (!_panicScreen) {
    return kIOReturnNotReady;
  }
  if (!begin) {
    startFramebuffer();
    return kIOReturnSuccess;
  }
  static const char sample[] =
    "panic(cpu 0 caller 0x00000000): XenonPE panic screen test, no panic happened.\n"
    "Latest stack backtrace for cpu 0:\n"
    "      Backtrace:\n"
    "         0x00095B9C 0x0009607C 0x00028C40 0x000AB8F4 0x000ADB98 0x00000000\n"
    "Kernel version:\n"
    "Darwin Kernel Version 8.11.0: Wed Oct 10 18:26:00 PDT 2007; root:xnu-792.24.17~1/RELEASE_PPC\n"
    "\n"
    "Font check: ABCDEFGHIJKLMNOPQRSTUVWXYZ abcdefghijklmnopqrstuvwxyz 0123456789 !\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~\n";
  stopFramebuffer();
  drawPanicScreen("XenonPE panic screen TEST (the desktop comes back in a few seconds)", sample);
  return kIOReturnSuccess;
}

//
// Masks and acknowledges the Xenos interrupt sources. Called by XenonInterruptController when
// vector 0x58 arrives and no driver handles it; XenonInterruptController cannot mask 0x58 itself.
//
void XenonPE::silenceGPUInterrupts(void) {
  if (_gpuMmioMem == NULL) {
    return;
  }
  OSWriteBigInt32(_gpuMmioMem, kXenosRegCpIntCntl, 0);
  OSWriteBigInt32(_gpuMmioMem, kXenosRegRbbmIntCntl, 0);
  OSWriteBigInt32(_gpuMmioMem, kXenosRegCpIntAck, 0xFFFFFFFF);
  OSWriteBigInt32(_gpuMmioMem, kXenosRegRbbmIntAck, 0xFFFFFFFF);
  __asm__ volatile ("sync");
}
