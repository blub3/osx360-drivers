#ifndef XENOS_WINDOW_COPY_H
#define XENOS_WINDOW_COPY_H
#include "XenosBlitABI.h"
#include "Offscreen.h"

#define XENOS_WINDOW_ARM "/Volumes/OSX360BOOT/XenosWindowRepaintTest/repaint-window.arm"

enum {
    kWindowCopySide = kXenosQuadUploadSide,
    kWindowCopyPixels = kWindowCopySide * kWindowCopySide,
    kFramebufferRowBytes = 1920 * sizeof(UInt32),
    kFramebufferBytes = 0x800000,
    kCommonWidth = 1280,
    kCommonHeight = 720,
    kGPUWindowCopyMagic = 0x58575231U
};

typedef struct { int sourceX, sourceY, targetX, targetY; } XenosWindowMove;
typedef struct {
    UInt32 magic;
    XenosWindowMove move;
    UInt32 hash;
} XenosWindowArm;
typedef struct {
    volatile UInt32 *base;
    unsigned long rowBytes;
    unsigned long width, height;
} XenosWindowPixels;
typedef struct {
    UInt32 connection, task, address, size;
    int (*close)(UInt32);
    int (*unmap)(UInt32, UInt32, UInt32, UInt32);
} XenosFramebufferMap;

int XenosMapFramebuffer(void *io, io_service_t framebuffer, XenosFramebufferMap *map);
void XenosUnmapFramebuffer(XenosFramebufferMap *map);
int XenosDecodeWindowMove(UInt32 type, UInt32 sourceType, const void *operation,
                          XenosWindowMove *move);
int XenosWindowMoveMatches(const XenosWindowMove *move, const XenosWindowArm *arm);
int XenosWindowSample(const XenosWindowPixels *screen, int x, int y,
                      UInt32 pixels[kWindowCopyPixels]);
UInt32 XenosWindowHash(const UInt32 pixels[kWindowCopyPixels]);
int XenosCompositeWindowCopy(const XenosWindowPixels *screen, const XenosWindowArm *arm,
                            XenosFence *fence, UInt32 *copied);
#endif
