#include "XenosWindowCopy.h"

extern UInt32 mach_task_self_;
extern int vm_region(UInt32, UInt32 *, UInt32 *, int, int *, UInt32 *, UInt32 *);
extern int mach_port_deallocate(UInt32, UInt32);

enum { kFramebufferShared = 1, kFramebufferVRAM = 110, kMapAnywhere = 1,
       kRegionBasicInfo = 10, kVMReadWrite = 3 };

typedef struct { short x, y, width, height; } TigerDeviceRect;
typedef struct {
    UInt32 count;
    TigerDeviceRect bounds;
    TigerDeviceRect rects[1];
} TigerDeviceRegion;
typedef char TigerRegionPrefix[ABI_OFFSET(TigerDeviceRegion, rects) == 12 ? 1 : -1];
typedef char TigerCopyRegionOffset[ABI_OFFSET(TigerCopyRegion, region) == 96 ? 1 : -1];

int XenosMapFramebuffer(void *io, io_service_t framebuffer, XenosFramebufferMap *map)
{
    int (*open)(UInt32, UInt32, UInt32, UInt32 *) = dlsym(io, "IOServiceOpen");
    int (*mapMemory)(UInt32, UInt32, UInt32, UInt32 *, UInt32 *, UInt32) =
        dlsym(io, "IOConnectMapMemory");
    *map = (XenosFramebufferMap){.task = mach_task_self_};
    map->close = dlsym(io, "IOServiceClose");
    map->unmap = dlsym(io, "IOConnectUnmapMemory");
    if (!open || !mapMemory || !map->unmap || !map->close ||
        open(framebuffer, map->task, kFramebufferShared, &map->connection))
        return 0;

    if (!mapMemory(map->connection, kFramebufferVRAM, map->task,
                   &map->address, &map->size, kMapAnywhere) &&
        map->address && map->size == kFramebufferBytes) {
        UInt32 address = map->address, size = 0, infoCount = 8, object = 0;
        int info[8] = {0};
        const int status = vm_region(map->task, &address, &size, kRegionBasicInfo,
                                     info, &infoCount, &object);
        if (object)
            mach_port_deallocate(map->task, object);
        if (!status && address == map->address && size >= map->size &&
            (info[0] & kVMReadWrite) == kVMReadWrite)
            return 1;
    }
    XenosUnmapFramebuffer(map);
    return 0;
}

void XenosUnmapFramebuffer(XenosFramebufferMap *map)
{
    if (map->address)
        map->unmap(map->connection, kFramebufferVRAM, map->task, map->address);
    if (map->connection)
        map->close(map->connection);
    *map = (XenosFramebufferMap){.connection = 0};
}

int XenosDecodeWindowMove(UInt32 type, UInt32 sourceType, const void *operation,
                          XenosWindowMove *move)
{
    sourceType &= ~kBlitDestinationBit;
    if (!operation)
        return 0;
    if (type == kBlitCopyRegion && sourceType == kBlitFramebuffer) {
        const TigerCopyRegion *copy = operation;
        const TigerDeviceRegion *region = copy->region;
        if (!region || region->count != 1)
            return 0;
        const TigerDeviceRect b = region->bounds, r = region->rects[0];
        if (b.x != r.x || b.y != r.y || b.width != r.width || b.height != r.height ||
            r.width != kWindowCopySide || r.height != kWindowCopySide)
            return 0;
        /* Tiger's region copy passes the destination origin of the region bounds. */
        *move = (XenosWindowMove){r.x, r.y, copy->deltaX, copy->deltaY};
        return 1;
    }
    if (type == kBlitCopy && (!sourceType || sourceType == kBlitFramebuffer)) {
        const TigerCopy *copy = operation;
        if (copy->count != 1 || copy->rectangle.width != kWindowCopySide ||
            copy->rectangle.height != kWindowCopySide)
            return 0;
        *move = (XenosWindowMove){copy->rectangle.sourceX, copy->rectangle.sourceY,
                                  copy->rectangle.x, copy->rectangle.y};
        return 1;
    }
    return 0;
}

int XenosWindowMoveMatches(const XenosWindowMove *move, const XenosWindowArm *arm)
{
    return arm->magic == kGPUWindowCopyMagic && move->sourceX == arm->move.sourceX &&
        move->sourceY == arm->move.sourceY && move->targetX == arm->move.targetX &&
        move->targetY == arm->move.targetY;
}

static int contains(const XenosWindowPixels *screen, int x, int y)
{
    return screen->base && x >= 0 && y >= 0 && screen->width >= kWindowCopySide &&
        screen->height >= kWindowCopySide && screen->rowBytes % sizeof(UInt32) == 0 &&
        screen->width <= screen->rowBytes / sizeof(UInt32) &&
        (unsigned long)x <= screen->width - kWindowCopySide &&
        (unsigned long)y <= screen->height - kWindowCopySide;
}

static volatile UInt32 *row(const XenosWindowPixels *screen, int x, int y)
{
    return screen->base + (unsigned long)y * (screen->rowBytes / sizeof(UInt32)) + x;
}

int XenosWindowSample(const XenosWindowPixels *screen, int x, int y,
                      UInt32 pixels[kWindowCopyPixels])
{
    if (!contains(screen, x, y))
        return 0;
    for (int dy = 0; dy < kWindowCopySide; ++dy) {
        const volatile UInt32 *src = row(screen, x, y + dy);
        for (int dx = 0; dx < kWindowCopySide; ++dx)
            pixels[dy * kWindowCopySide + dx] = src[dx];
    }
    return 1;
}

UInt32 XenosWindowHash(const UInt32 pixels[kWindowCopyPixels])
{
    UInt32 hash = 2166136261U;
    for (int i = 0; i < kWindowCopyPixels; ++i) {
        hash ^= pixels[i] & 0xffffffU;
        hash *= 16777619U;
    }
    return hash;
}

int XenosCompositeWindowCopy(const XenosWindowPixels *screen, const XenosWindowArm *arm,
                            XenosFence *fence, UInt32 *copied)
{
    const XenosWindowMove *move = &arm->move;
    UInt32 source[kWindowCopyPixels], originalTarget[kWindowCopyPixels];
    *copied = 0;
    if (!contains(screen, move->sourceX, move->sourceY) ||
        !contains(screen, move->targetX, move->targetY) ||
        (move->sourceX < move->targetX + kWindowCopySide && move->targetX < move->sourceX + kWindowCopySide &&
         move->sourceY < move->targetY + kWindowCopySide && move->targetY < move->sourceY + kWindowCopySide) ||
        !XenosWindowSample(screen, move->sourceX, move->sourceY, source) ||
        !XenosWindowSample(screen, move->targetX, move->targetY, originalTarget) ||
        XenosWindowHash(source) != arm->hash)
        return 0;

    XenosQuadGeometryUpload upload;
    upload.geometry = xenosQuadMakeGeometry(0, 0, kWindowCopySide, kWindowCopySide,
                                             0, 0, kWindowCopySide, kWindowCopySide);
    for (int i = 0; i < kWindowCopyPixels; ++i) {
        upload.rgb[i * 3] = (unsigned char)(source[i] >> 16);
        upload.rgb[i * 3 + 1] = (unsigned char)(source[i] >> 8);
        upload.rgb[i * 3 + 2] = (unsigned char)source[i];
    }
    XenosBackend backend = {.connection = 0};
    if (!XenosOpenBackend(&backend))
        return 0;
    UInt32 result[kXenosQuadReadbackPixels];
    const int rendered = XenosRenderQuad(&backend, &upload, 0, fence, result);
    XenosCloseBackend(&backend);
    if (!rendered)
        return 0;

    /* Verify the entire uploaded tile before any write to the destination. */
    for (int y = 0; y < kWindowCopySide; ++y)
        for (int x = 0; x < kWindowCopySide; ++x)
            if (((source[y * kWindowCopySide + x] ^ result[y * kXenosQuadTargetSide + x]) & 0xffffffU))
                return 0;

    UInt32 current[kWindowCopyPixels];
    if (!XenosWindowSample(screen, move->sourceX, move->sourceY, current))
        return 0;
    for (int i = 0; i < kWindowCopyPixels; ++i)
        if (current[i] != source[i])
            return 0;
    if (!XenosWindowSample(screen, move->targetX, move->targetY, current))
        return 0;
    for (int i = 0; i < kWindowCopyPixels; ++i)
        if (current[i] != originalTarget[i])
            return 0;

    for (int y = 0; y < kWindowCopySide; ++y) {
        volatile UInt32 *dst = row(screen, move->targetX, move->targetY + y);
        for (int x = 0; x < kWindowCopySide; ++x)
            dst[x] = (source[y * kWindowCopySide + x] & 0xff000000U) |
                     (result[y * kXenosQuadTargetSide + x] & 0xffffffU);
    }
    __asm__ volatile("sync" ::: "memory");
    if (!XenosWindowSample(screen, move->targetX, move->targetY, current))
        return 0;
    for (int i = 0; i < kWindowCopyPixels; ++i)
        *copied += current[i] == source[i];
    return *copied == kWindowCopyPixels;
}
