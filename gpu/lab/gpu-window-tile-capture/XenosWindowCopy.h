#ifndef XENOS_WINDOW_COPY_H
#define XENOS_WINDOW_COPY_H
#include "XenosBlitABI.h"
#include "Offscreen.h"
#include "XenosWindowAperture.h"

#define XENOS_WINDOW_ARM "/Volumes/OSX360BOOT/XenosWindowTileTest/repaint-window.arm"

enum {
    kWindowCopySide = kXenosQuadUploadSide,
    kWindowCopyPixels = kWindowCopySide * kWindowCopySide,
    kFramebufferRowBytes = 1920 * sizeof(UInt32),
    kFramebufferBytes = 0x800000,
    kCommonWidth = 1280,
    kCommonHeight = 720,
    kFullCopyBatchLimit = 8,
    kBatchFenceTextBytes = kFullCopyBatchLimit * 11 + 1,
    kFullCopyPixelLimit = kFullCopyBatchLimit * kXenosQuadReadbackPixels,
    kWindowCopyRectLimit = 32,
    kWindowCopyAreaLimit = 128 * 128,
    kGPUWindowCopyMagic = 0x58575431U
};

typedef struct { int sourceX, sourceY, targetX, targetY; } XenosWindowMove;
typedef struct { XenosWindowMove move; int width, height; } XenosCopyRectangle;
typedef struct { UInt32 count, area; XenosCopyRectangle rects[kWindowCopyRectLimit]; } XenosCopyPlan;
typedef struct {
    UInt32 batches, firstFence, ticket, fence;
    UInt32 fences[kFullCopyBatchLimit];
    UInt32 gpuPixels, screenPixels, hash, elapsedMs;
    int committed;
} XenosFullCopy;
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
    UInt32 connection, task, address, size, memoryType;
    int (*close)(UInt32);
    int (*unmap)(UInt32, UInt32, UInt32, UInt32);
} XenosFramebufferMap;

int XenosCaptureTile(XenosBackend *, const XenosQuadTileUpload *, XenosFence *, UInt32 *argb);
int XenosTryCaptureTile(XenosBackend *, const XenosQuadTileUpload *, XenosFence *, UInt32 *argb,
                        unsigned submitAttempts);
void XenosFormatBatchFences(const XenosFullCopy *copy, char text[kBatchFenceTextBytes]);
int XenosMapFramebuffer(void *io, io_service_t framebuffer, XenosFramebufferMap *map);
const char *XenosMapWindowAperture(void *io, io_service_t framebuffer, XenosFramebufferMap *map,
                                  UInt32 *status);
void XenosUnmapFramebuffer(XenosFramebufferMap *map);
int XenosDecodeWindowCopy(UInt32 type, UInt32 sourceType, const void *operation,
                          XenosCopyPlan *plan);
int XenosWindowCopyCovers(const XenosCopyPlan *plan, const XenosWindowArm *arm);
int XenosUnarmedOverlapEligible(const XenosCopyPlan *plan);
int XenosWindowSample(const XenosWindowPixels *screen, int x, int y,
                      UInt32 pixels[kWindowCopyPixels]);
UInt32 XenosWindowHash(const UInt32 pixels[kWindowCopyPixels]);
const char *XenosCompositeWindowCopy(const XenosWindowPixels *screen, const XenosCopyPlan *plan,
                                     const XenosWindowArm *arm, XenosFullCopy *copy);
const char *XenosCompositeUnarmedCopy(const XenosWindowPixels *screen, const XenosCopyPlan *plan,
                                     XenosFullCopy *copy);
const char *XenosCompositeUnarmedOverlap(const XenosWindowPixels *screen, const XenosCopyPlan *plan,
                                        XenosFullCopy *copy);
#endif
