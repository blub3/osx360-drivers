#include "XenosWindowCopy.h"

extern UInt32 mach_task_self_;
extern int memcmp(const void *, const void *, unsigned long);
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

static const char *mapFramebuffer(void *io, io_service_t framebuffer, UInt32 connectionType,
                                  UInt32 memoryType, XenosFramebufferMap *map, UInt32 *status)
{
    int (*open)(UInt32, UInt32, UInt32, UInt32 *) = dlsym(io, "IOServiceOpen");
    int (*mapMemory)(UInt32, UInt32, UInt32, UInt32 *, UInt32 *, UInt32) =
        dlsym(io, "IOConnectMapMemory");
    *map = (XenosFramebufferMap){.task = mach_task_self_, .memoryType = memoryType};
    map->close = dlsym(io, "IOServiceClose");
    map->unmap = dlsym(io, "IOConnectUnmapMemory");
    *status = kBlitUnsupported;
    if (!open || !mapMemory || !map->unmap || !map->close || !map->task)
        return "map-symbols";

    *status = open(framebuffer, map->task, connectionType, &map->connection);
    if (*status || !map->connection)
        return "map-open";

    const char *failure = "map-memory";
    *status = mapMemory(map->connection, map->memoryType, map->task,
                        &map->address, &map->size, kMapAnywhere);
    if (*status)
        goto failed;

    failure = "map-size";
    if (!map->address || map->size != kFramebufferBytes)
        goto failed;

    UInt32 address = map->address, size = 0, infoCount = 8, object = 0;
    int info[8] = {0};
    *status = vm_region(map->task, &address, &size, kRegionBasicInfo, info, &infoCount, &object);
    if (object)
        mach_port_deallocate(map->task, object);
    failure = "map-region";
    if (!*status && address == map->address && size >= map->size &&
        (info[0] & kVMReadWrite) == kVMReadWrite)
        return 0;
failed:
    XenosUnmapFramebuffer(map);
    return failure;
}

int XenosMapFramebuffer(void *io, io_service_t framebuffer, XenosFramebufferMap *map)
{
    UInt32 status;
    return !mapFramebuffer(io, framebuffer, kFramebufferShared, kFramebufferVRAM, map, &status);
}

const char *XenosMapWindowAperture(void *io, io_service_t framebuffer, XenosFramebufferMap *map,
                                  UInt32 *status)
{
    return mapFramebuffer(io, framebuffer, kXenosWindowApertureClient,
                          kXenosWindowApertureMemory, map, status);
}

void XenosUnmapFramebuffer(XenosFramebufferMap *map)
{
    if (map->address)
        map->unmap(map->connection, map->memoryType, map->task, map->address);
    if (map->connection)
        map->close(map->connection);
    *map = (XenosFramebufferMap){.connection = 0};
}

static int validRectangle(const XenosCopyRectangle *rect, UInt32 *area)
{
    const XenosWindowMove *move = &rect->move;
    return rect->width > 0 && rect->width <= 128 && rect->height > 0 && rect->height <= 128 &&
        move->sourceX >= 0 && move->sourceY >= 0 && move->targetX >= 0 && move->targetY >= 0 &&
        move->sourceX <= kCommonWidth - rect->width && move->targetX <= kCommonWidth - rect->width &&
        move->sourceY <= kCommonHeight - rect->height && move->targetY <= kCommonHeight - rect->height &&
        (*area += rect->width * rect->height) <= kWindowCopyAreaLimit;
}

int XenosDecodeWindowCopy(UInt32 type, UInt32 sourceType, const void *operation,
                          XenosCopyPlan *plan)
{
    sourceType &= ~kBlitDestinationBit;
    plan->count = plan->area = 0;
    if (!operation)
        return 0;
    if (type == kBlitCopyRegion && sourceType == kBlitFramebuffer) {
        const TigerCopyRegion *copy = operation;
        const TigerDeviceRegion *region = copy->region;
        if (!region || region->count > kWindowCopyRectLimit ||
            copy->deltaX < 0 || copy->deltaX > kCommonWidth ||
            copy->deltaY < 0 || copy->deltaY > kCommonHeight)
            return 0;
        const TigerDeviceRect bounds = region->bounds;
        plan->count = region->count ? region->count : 1;
        for (UInt32 i = 0; i < plan->count; ++i) {
            const TigerDeviceRect r = region->count ? region->rects[i] : bounds;
            if (r.x < bounds.x || r.y < bounds.y ||
                r.x + r.width > bounds.x + bounds.width ||
                r.y + r.height > bounds.y + bounds.height)
                return 0;
            plan->rects[i] = (XenosCopyRectangle){
                {r.x, r.y, copy->deltaX + r.x - bounds.x, copy->deltaY + r.y - bounds.y},
                r.width, r.height
            };
        }
    } else if (type == kBlitCopy && (!sourceType || sourceType == kBlitFramebuffer)) {
        const TigerCopy *copy = operation;
        if (!copy->count || copy->count > kWindowCopyRectLimit)
            return 0;
        plan->count = copy->count;
        for (UInt32 i = 0; i < plan->count; ++i) {
            const TigerCopyRect *r = &copy->rects[i];
            plan->rects[i] = (XenosCopyRectangle){
                {r->sourceX, r->sourceY, r->x, r->y}, r->width, r->height
            };
        }
    } else {
        return 0;
    }
    for (UInt32 i = 0; i < plan->count; ++i)
        if (!validRectangle(&plan->rects[i], &plan->area))
            return 0;
    return 1;
}

int XenosWindowCopyCovers(const XenosCopyPlan *plan, const XenosWindowArm *arm)
{
    unsigned char covered[kWindowCopyPixels] = {0};
    const XenosWindowMove *tile = &arm->move;
    if (arm->magic != kGPUWindowCopyMagic || tile->sourceX < 0 || tile->sourceY < 0 ||
        tile->targetX < 0 || tile->targetY < 0 ||
        tile->sourceX > kCommonWidth - kWindowCopySide || tile->targetX > kCommonWidth - kWindowCopySide ||
        tile->sourceY > kCommonHeight - kWindowCopySide || tile->targetY > kCommonHeight - kWindowCopySide ||
        (tile->sourceX == tile->targetX && tile->sourceY == tile->targetY))
        return 0;
    for (UInt32 i = 0; i < plan->count; ++i) {
        const XenosCopyRectangle *rect = &plan->rects[i];
        const XenosWindowMove *move = &rect->move;
        if (move->targetX - move->sourceX != tile->targetX - tile->sourceX ||
            move->targetY - move->sourceY != tile->targetY - tile->sourceY)
            return 0;
        for (int y = 0; y < kWindowCopySide; ++y)
            for (int x = 0; x < kWindowCopySide; ++x)
                if (tile->sourceX + x >= move->sourceX && tile->sourceX + x < move->sourceX + rect->width &&
                    tile->sourceY + y >= move->sourceY && tile->sourceY + y < move->sourceY + rect->height)
                    covered[y * kWindowCopySide + x] = 1;
    }
    for (int i = 0; i < kWindowCopyPixels; ++i)
        if (!covered[i])
            return 0;
    return 1;
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

typedef struct { UInt32 source, target; } PixelPair;

static int snapshot(const XenosWindowPixels *screen, const XenosCopyPlan *plan,
                    PixelPair *pairs, int verify)
{
    UInt32 index = 0;
    for (UInt32 i = 0; i < plan->count; ++i) {
        const XenosCopyRectangle *rect = &plan->rects[i];
        const XenosWindowMove *move = &rect->move;
        for (int y = 0; y < rect->height; ++y) {
            volatile UInt32 *from = row(screen, move->sourceX, move->sourceY + y);
            volatile UInt32 *to = row(screen, move->targetX, move->targetY + y);
            for (int x = 0; x < rect->width; ++x, ++index) {
                if (verify) {
                    if (pairs[index].source != from[x] || pairs[index].target != to[x])
                        return 0;
                } else {
                    pairs[index] = (PixelPair){from[x], to[x]};
                }
            }
        }
    }
    return index == plan->area;
}

static void publish(const XenosWindowPixels *screen, const XenosCopyPlan *plan,
                    const XenosWindowMove *tile, const PixelPair *pairs, const UInt32 *result)
{
    UInt32 index = 0;
    for (UInt32 i = 0; i < plan->count; ++i) {
        const XenosCopyRectangle *rect = &plan->rects[i];
        const XenosWindowMove *move = &rect->move;
        for (int y = 0; y < rect->height; ++y) {
            volatile UInt32 *dst = row(screen, move->targetX, move->targetY + y);
            const int tileY = move->targetY + y - tile->targetY;
            for (int x = 0; x < rect->width; ++x, ++index) {
                const int tileX = move->targetX + x - tile->targetX;
                UInt32 pixel = pairs[index].source;
                if (tileY >= 0 && tileY < kWindowCopySide && tileX >= 0 && tileX < kWindowCopySide)
                    pixel = (pixel & 0xff000000U) |
                            (result[tileY * kXenosQuadTargetSide + tileX] & 0xffffffU);
                dst[x] = pixel;
            }
        }
    }
    __asm__ volatile("sync" ::: "memory");
}

const char *XenosCompositeWindowCopy(const XenosWindowPixels *screen, const XenosCopyPlan *plan,
                                     const XenosWindowArm *arm, XenosFence *fence, UInt32 *copied, int *committed)
{
    const XenosWindowMove *tile = &arm->move;
    *copied = 0;
    *committed = 0;
    if (!XenosWindowCopyCovers(plan, arm) || !screen->base ||
        screen->rowBytes % sizeof(UInt32) || screen->width > screen->rowBytes / sizeof(UInt32))
        return "geometry";
    for (UInt32 i = 0; i < plan->count; ++i) {
        const XenosCopyRectangle *rect = &plan->rects[i];
        const XenosWindowMove *move = &rect->move;
        if ((unsigned long)(move->sourceX + rect->width) > screen->width ||
            (unsigned long)(move->targetX + rect->width) > screen->width ||
            (unsigned long)(move->sourceY + rect->height) > screen->height ||
            (unsigned long)(move->targetY + rect->height) > screen->height)
            return "bounds";
    }
    PixelPair *pairs = calloc(plan->area, sizeof(*pairs));
    if (!pairs)
        return "memory";
    const char *failure = "source-hash";
    UInt32 source[kWindowCopyPixels];
    if (!snapshot(screen, plan, pairs, 0) ||
        !XenosWindowSample(screen, tile->sourceX, tile->sourceY, source) ||
        XenosWindowHash(source) != arm->hash)
        goto done;

    XenosQuadGeometryUpload upload;
    upload.geometry = xenosQuadMakeGeometry(0, 0, kWindowCopySide, kWindowCopySide,
                                             0, 0, kWindowCopySide, kWindowCopySide);
    for (int i = 0; i < kWindowCopyPixels; ++i) {
        upload.rgb[i * 3] = (unsigned char)(source[i] >> 16);
        upload.rgb[i * 3 + 1] = (unsigned char)(source[i] >> 8);
        upload.rgb[i * 3 + 2] = (unsigned char)source[i];
    }
    XenosBackend backend = {.connection = 0};
    failure = "gpu-open";
    if (!XenosOpenBackend(&backend))
        goto done;
    UInt32 result[kXenosQuadReadbackPixels];
    const int rendered = XenosRenderQuad(&backend, &upload, 0, fence, result);
    XenosCloseBackend(&backend);
    failure = "gpu-fence";
    if (!rendered)
        goto done;
    failure = "gpu-pixels";
    for (int y = 0; y < kWindowCopySide; ++y)
        for (int x = 0; x < kWindowCopySide; ++x)
            if ((source[y * kWindowCopySide + x] ^ result[y * kXenosQuadTargetSide + x]) & 0xffffffU)
                goto done;

    failure = "framebuffer-changed";
    if (!snapshot(screen, plan, pairs, 1))
        goto done;
    UInt32 current[kWindowCopyPixels];
    if (!XenosWindowSample(screen, tile->sourceX, tile->sourceY, current) ||
        memcmp(source, current, sizeof(source)))
        goto done;

    publish(screen, plan, tile, pairs, result);
    *committed = 1;
    failure = "screen-readback";
    if (!XenosWindowSample(screen, tile->targetX, tile->targetY, current))
        goto done;
    for (int i = 0; i < kWindowCopyPixels; ++i)
        *copied += current[i] == source[i];
    if (*copied == kWindowCopyPixels)
        failure = 0;
done:
    free(pairs);
    return failure;
}
