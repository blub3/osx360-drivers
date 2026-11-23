#include "XenosWindowCopy.h"

extern UInt32 mach_task_self_;
extern int memcmp(const void *, const void *, unsigned long);
extern int vm_region(UInt32, UInt32 *, UInt32 *, int, int *, UInt32 *, UInt32 *);
extern int mach_port_deallocate(UInt32, UInt32);
extern unsigned long long mach_absolute_time(void);
extern int snprintf(char *, unsigned long, const char *, ...);

typedef struct { UInt32 numer, denom; } MachTimebase;
extern int mach_timebase_info(MachTimebase *);

enum { kBatchBudgetMs = 8000 };

static UInt32 batchElapsedMs(unsigned long long start, const MachTimebase *timebase)
{
    return (UInt32)((mach_absolute_time() - start) * timebase->numer /
                    timebase->denom / 1000000ULL);
}

void XenosFormatBatchFences(const XenosFullCopy *copy, char text[kBatchFenceTextBytes])
{
    unsigned long length = 0;
    text[0] = 0;
    for (UInt32 i = 0; i < copy->batches; ++i) {
        const int written = snprintf(text + length, kBatchFenceTextBytes - length,
                                     i ? ",%u" : "%u", copy->fences[i]);
        if (written < 0 || (unsigned long)written >= kBatchFenceTextBytes - length)
            break;
        length += written;
    }
}

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
    for (UInt32 i = 0; i < plan->count; ++i) {
        const XenosCopyRectangle *a = &plan->rects[i];
        if (!validRectangle(a, &plan->area))
            return 0;
        for (UInt32 j = 0; j < i; ++j) {
            const XenosCopyRectangle *b = &plan->rects[j];
            if (a->move.sourceX < b->move.sourceX + b->width &&
                b->move.sourceX < a->move.sourceX + a->width &&
                a->move.sourceY < b->move.sourceY + b->height &&
                b->move.sourceY < a->move.sourceY + a->height)
                return 0;
        }
    }
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

typedef struct { UInt32 source, target, gpuRGB; } PixelPair;

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
                    pairs[index] = (PixelPair){from[x], to[x], 0};
                }
            }
        }
    }
    return index == plan->area;
}

static const char *renderRegion(XenosBackend *backend, PixelPair *pairs, UInt32 area,
                                XenosFullCopy *copy)
{
    MachTimebase timebase;
    if (mach_timebase_info(&timebase) || !timebase.numer || !timebase.denom)
        return "gpu-clock";
    const unsigned long long start = mach_absolute_time();
    // Pack only covered clip pixels into each texture, including partial tiles.
    for (UInt32 first = 0; first < area; first += kWindowCopyPixels) {
        if (copy->elapsedMs >= kBatchBudgetMs)
            return "gpu-budget";
        const UInt32 left = area - first;
        const UInt32 count = left < kWindowCopyPixels ? left : kWindowCopyPixels;
        XenosQuadGeometryUpload upload = {.rgb = {0}};
        upload.geometry = xenosQuadMakeGeometry(0, 0, kWindowCopySide, kWindowCopySide,
                                                0, 0, kWindowCopySide, kWindowCopySide);
        for (UInt32 i = 0; i < count; ++i) {
            const UInt32 pixel = pairs[first + i].source;
            upload.rgb[i * 3] = (unsigned char)(pixel >> 16);
            upload.rgb[i * 3 + 1] = (unsigned char)(pixel >> 8);
            upload.rgb[i * 3 + 2] = (unsigned char)pixel;
        }
        UInt32 result[kXenosQuadReadbackPixels];
        XenosFence fence = {.ticket = 0};
        const int completed = XenosRenderQuad(backend, &upload, 0, &fence, result);
        copy->elapsedMs = batchElapsedMs(start, &timebase);
        if (!completed || fence.ticket != fence.fence ||
            (copy->batches && fence.fence != copy->fence + 1))
            return "gpu-fence";
        if (!copy->batches)
            copy->firstFence = fence.fence;
        copy->fences[copy->batches++] = fence.fence;
        copy->ticket = fence.ticket;
        copy->fence = fence.fence;
        for (UInt32 i = 0; i < count; ++i) {
            const UInt32 y = i / kWindowCopySide, x = i % kWindowCopySide;
            const UInt32 rgb = result[y * kXenosQuadTargetSide + x] & 0xffffffU;
            PixelPair *pair = &pairs[first + i];
            if ((pair->source & 0xffffffU) != rgb)
                return "gpu-pixels";
            pair->gpuRGB = rgb;
            ++copy->gpuPixels;
        }
    }
    return copy->elapsedMs >= kBatchBudgetMs ? "gpu-budget" : 0;
}

static void publish(const XenosWindowPixels *screen, const XenosCopyPlan *plan,
                    const PixelPair *pairs)
{
    UInt32 index = 0;
    for (UInt32 i = 0; i < plan->count; ++i) {
        const XenosCopyRectangle *rect = &plan->rects[i];
        for (int y = 0; y < rect->height; ++y) {
            volatile UInt32 *dst = row(screen, rect->move.targetX, rect->move.targetY + y);
            for (int x = 0; x < rect->width; ++x, ++index)
                dst[x] = (pairs[index].source & 0xff000000U) | pairs[index].gpuRGB;
        }
    }
    __asm__ volatile("sync" ::: "memory");
}

static void verifyPublished(const XenosWindowPixels *screen, const XenosCopyPlan *plan,
                            const PixelPair *pairs, XenosFullCopy *copy)
{
    UInt32 index = 0;
    UInt32 hash = 2166136261U;
    for (UInt32 i = 0; i < plan->count; ++i) {
        const XenosCopyRectangle *rect = &plan->rects[i];
        for (int y = 0; y < rect->height; ++y) {
            const volatile UInt32 *dst = row(screen, rect->move.targetX, rect->move.targetY + y);
            for (int x = 0; x < rect->width; ++x, ++index) {
                const UInt32 observed = dst[x];
                copy->screenPixels += observed == pairs[index].source;
                hash ^= observed & 0xffffffU;
                hash *= 16777619U;
            }
        }
    }
    copy->hash = hash;
}

const char *XenosCompositeWindowCopy(const XenosWindowPixels *screen, const XenosCopyPlan *plan,
                                     const XenosWindowArm *arm, XenosFullCopy *copy)
{
    const XenosWindowMove *tile = &arm->move;
    *copy = (XenosFullCopy){.batches = 0};
    if (!plan->area || plan->area > kFullCopyPixelLimit)
        return "region-limit";
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
    UInt32 source[kWindowCopyPixels], current[kWindowCopyPixels];
    if (!snapshot(screen, plan, pairs, 0) ||
        !XenosWindowSample(screen, tile->sourceX, tile->sourceY, source) ||
        XenosWindowHash(source) != arm->hash)
        goto done;

    XenosBackend backend = {.connection = 0};
    failure = "gpu-open";
    if (!XenosOpenBackend(&backend))
        goto done;
    failure = renderRegion(&backend, pairs, plan->area, copy);
    XenosCloseBackend(&backend);
    if (failure)
        goto done;

    failure = "framebuffer-changed";
    if (!snapshot(screen, plan, pairs, 1) ||
        !XenosWindowSample(screen, tile->sourceX, tile->sourceY, current) ||
        memcmp(source, current, sizeof(source)))
        goto done;

    publish(screen, plan, pairs);
    copy->committed = 1;
    verifyPublished(screen, plan, pairs, copy);
    failure = copy->screenPixels == plan->area ? 0 : "screen-readback";
done:
    free(pairs);
    return failure;
}
