#ifndef XENOS_WINDOW_COPY_H
#define XENOS_WINDOW_COPY_H
#include "XenosBlitABI.h"
#include "Offscreen.h"

#define XENOS_WINDOW_ARM "/Volumes/OSX360BOOT/XenosWindowRetryTest/repaint-window.arm"

enum {
    kWindowCopySide = kXenosQuadUploadSide,
    kWindowCopyPixels = kWindowCopySide * kWindowCopySide,
    kFramebufferRowBytes = 1920 * sizeof(UInt32),
    kFramebufferBytes = 0x800000,
    kCommonWidth = 1280,
    kCommonHeight = 720,
    kWindowCopyRectLimit = 32,
    kWindowCopyAreaLimit = 128 * 128,
    kGPUWindowCopyMagic = 0x58575231U
};

typedef struct { int sourceX, sourceY, targetX, targetY; } XenosWindowMove;
typedef struct { XenosWindowMove move; int width, height; } XenosCopyRectangle;
typedef struct { UInt32 count, area; XenosCopyRectangle rects[kWindowCopyRectLimit]; } XenosCopyPlan;
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
int XenosDecodeWindowCopy(UInt32 type, UInt32 sourceType, const void *operation,
                          XenosCopyPlan *plan);
int XenosWindowCopyCovers(const XenosCopyPlan *plan, const XenosWindowArm *arm);
int XenosWindowSample(const XenosWindowPixels *screen, int x, int y,
                      UInt32 pixels[kWindowCopyPixels]);
UInt32 XenosWindowHash(const UInt32 pixels[kWindowCopyPixels]);
const char *XenosCompositeWindowCopy(const XenosWindowPixels *screen, const XenosCopyPlan *plan,
                                     const XenosWindowArm *arm, XenosFence *fence, UInt32 *copied, int *committed);
#endif
