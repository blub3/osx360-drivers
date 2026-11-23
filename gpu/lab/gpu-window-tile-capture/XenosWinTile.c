#include "XenosWindowCopy.h"

extern int _NSGetExecutablePath(char *, UInt32 *);
extern char *realpath(const char *, char *);
extern int getpid(void);
extern unsigned long long mach_absolute_time(void);
typedef struct { UInt32 numer, denom; } MachTimebase;
extern int mach_timebase_info(MachTimebase *);
#ifndef XENOS_UNARMED_BLIT
extern int open(const char *, int, ...);
extern int close(int);
extern long read(int, void *, unsigned long);
enum { kReadOnly = 0, kNonBlocking = 4, kNoFollow = 0x100 };
#endif
extern int memcmp(const void *, const void *, unsigned long);
extern void syslog(int, const char *, ...);
extern _Bool OSAtomicCompareAndSwap32(int, int, volatile int *);
extern int OSSpinLockTry(volatile int *);
extern void OSSpinLockUnlock(volatile int *);

enum { kNotice = 5 };

#ifdef XENOS_UNARMED_BLIT
#define XENOS_BLIT_NAME "XenosWinAuto"
#define XENOS_BLIT_ENTRY XenosWinAutoFactory
#else
#define XENOS_BLIT_NAME "XenosWinTile"
#define XENOS_BLIT_ENTRY XenosWinTileFactory
#endif

typedef struct {
    const TigerBlitInterface *interface;
    UInt32 refs;
    CFRef factory;
    void *cf, *io;
    io_service_t framebuffer;
    int windowServer;
    volatile int beamReported;
#ifdef XENOS_UNARMED_BLIT
    unsigned long long cooldownTicks, nextSample;
#endif
    int (*conforms)(io_object_t, const char *);
    int (*retainObject)(io_object_t);
    int (*releaseObject)(io_object_t);
    int (*findAccelerator)(io_service_t, io_service_t *, UInt32 *);
    void (*removeInstance)(CFRef);
    void (*releaseCF)(CFRef);
} Engine;

static const TigerBlitInterface interface;
static volatile int sampleLock;
#ifndef XENOS_UNARMED_BLIT
static int attempted;
static volatile int traceCount;
#endif

static int isWindowServer(void)
{
    const char *path = "/System/Library/Frameworks/ApplicationServices.framework/"
        "Frameworks/CoreGraphics.framework/Resources/WindowServer";
    char name[1024], actual[1024], expected[1024];
    UInt32 size = sizeof(name);
    return !_NSGetExecutablePath(name, &size) &&
        realpath(name, actual) && realpath(path, expected) && !strcmp(actual, expected);
}

static UInt32 retain(void *self)
{
    return ++((Engine *)self)->refs;
}

static UInt32 release(void *self)
{
    Engine *engine = self;
    UInt32 remaining = --engine->refs;
    if (!remaining) {
        if (engine->framebuffer)
            engine->releaseObject(engine->framebuffer);
        engine->removeInstance(engine->factory);
        engine->releaseCF(engine->factory);
        dlclose(engine->io);
        dlclose(engine->cf);
        free(engine);
    }
    return remaining;
}

static int query(void *self, TigerUUID iid, void **out)
{
    if (!out)
        return 0x80000003;
    *out = 0;
    if (memcmp(&iid, &graphicsInterface, sizeof(iid)) &&
        memcmp(&iid, &unknownInterface, sizeof(iid)))
        return 0x80000004;
    retain(self);
    *out = self;
    return 0;
}

static int probe(void *self, CFRef properties, io_service_t framebuffer, int *score)
{
    Engine *engine = self;
    (void)properties;
    if (!score || !framebuffer || !engine->conforms(framebuffer, "XenonFramebufferTest"))
        return kBlitUnsupported;
    *score = 100;
    return 0;
}

static int start(void *self, CFRef properties, io_service_t framebuffer)
{
    Engine *engine = self;
    int score;
    io_service_t accelerator = 0;
    UInt32 index = 0;
    if (engine->framebuffer || probe(self, properties, framebuffer, &score))
        return kBlitUnsupported;
    int status = engine->findAccelerator(framebuffer, &accelerator, &index);
    if (status || !accelerator || index || !engine->conforms(accelerator, "XenosDisplayAccelerator")) {
        if (accelerator)
            engine->releaseObject(accelerator);
        return status ? status : (int)kBlitUnsupported;
    }
    engine->releaseObject(accelerator);
    if (engine->retainObject(framebuffer))
        return kBlitNoResources;
    engine->framebuffer = framebuffer;
    if (engine->windowServer) {
#ifdef XENOS_UNARMED_BLIT
        MachTimebase timebase;
        if (!mach_timebase_info(&timebase) && timebase.numer && timebase.denom) {
            engine->cooldownTicks = 1000000000ULL * timebase.denom / timebase.numer;
            engine->nextSample = mach_absolute_time() + 5 * engine->cooldownTicks;
        }
#endif
        syslog(kNotice, XENOS_BLIT_NAME " start pid=%d framebuffer_index=%u", getpid(), index);
    }
    return 0;
}

static int stop(void *self)
{
    Engine *engine = self;
    if (engine->framebuffer)
        engine->releaseObject(engine->framebuffer);
    engine->framebuffer = 0;
    return 0;
}

static int idle(void *self, UInt32 options)
{
    (void)self;
    (void)options;
    return 0;
}

static int capabilities(void *self, UInt32 select, CFRef *out)
{
    (void)self;
    (void)select;
    if (out)
        *out = 0;
    return kBlitUnsupported;
}

static int destination(void *self, UInt32 options, const void *surface)
{
    (void)self;
    /* Recognize the framebuffer destination used during IOPS allocation. */
    return !options && !surface ? 0 : (int)kBlitUnsupported;
}

static int beamPosition(void *self, UInt32 options, int *position)
{
    Engine *engine = self;
    (void)options;
    if (!position)
        return kBlitBadArgument;
    if (engine->windowServer && OSAtomicCompareAndSwap32(0, 1, &engine->beamReported))
        syslog(kNotice, XENOS_BLIT_NAME " beam pid=%d status=0x%08x", getpid(), (UInt32)kBlitUnsupported);
    /* Tiger's startup fade applies the transfer immediately on a nonzero status. */
    return kBlitUnsupported;
}

static int synchronize(void *self, UInt32 options, UInt32 x, UInt32 y, UInt32 width, UInt32 height)
{
    (void)self;
    (void)options;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
    return kBlitUnsupported;
}

static int unsupportedSurface(void *self, UInt32 options, TigerBlitSurface *surface)
{
    (void)self;
    (void)options;
    (void)surface;
    return kBlitUnsupported;
}

static int allocateSurface(void *self, UInt32 options, TigerBlitSurface *surface, void *cgsID)
{
    (void)cgsID;
    return unsupportedSurface(self, options, surface);
}

static int surfaceOutput(void *self, UInt32 options, TigerBlitSurface *surface, UInt32 *output)
{
    (void)output;
    return unsupportedSurface(self, options, surface);
}

#ifdef XENOS_UNARMED_BLIT
static int blit(void *self, UInt32 options, UInt32 type, UInt32 sourceType,
                const void *operation, const void *source)
{
    Engine *engine = self;
    (void)options;
    if (!engine->windowServer || !engine->framebuffer || !engine->cooldownTicks ||
        source || type != kBlitCopyRegion)
        return kBlitNoResources;

    XenosCopyPlan plan;
    if (!XenosDecodeWindowCopy(type, sourceType, operation, &plan) || plan.count != 1 ||
        plan.area < 256 || plan.area > kXenosQuadReadbackPixels)
        return kBlitNoResources;
    const XenosCopyRectangle *rect = &plan.rects[0];
    const XenosWindowMove *move = &rect->move;
    if ((move->sourceX == move->targetX && move->sourceY == move->targetY) ||
        !OSSpinLockTry(&sampleLock))
        return kBlitNoResources;
    if (mach_absolute_time() < engine->nextSample) {
        OSSpinLockUnlock(&sampleLock);
        return kBlitNoResources;
    }

    XenosFramebufferMap map;
    XenosFullCopy copy = {.batches = 0};
    UInt32 mapStatus = 0;
    const char *failure = XenosMapWindowAperture(engine->io, engine->framebuffer, &map, &mapStatus);
    if (!failure) {
        const XenosWindowPixels screen = {(volatile UInt32 *)map.address, kFramebufferRowBytes,
                                           kCommonWidth, kCommonHeight};
        failure = XenosCompositeUnarmedCopy(&screen, &plan, &copy);
        XenosUnmapFramebuffer(&map);
    }
    engine->nextSample = mach_absolute_time() + engine->cooldownTicks;
    OSSpinLockUnlock(&sampleLock);

    syslog(kNotice, XENOS_BLIT_NAME " %s pid=%d src=%d,%d dst=%d,%d size=%d,%d area=%u "
           "ticket=%u fence=%u gpu_pixels=%u screen_pixels=%u hash=%08x gpu_ms=%u "
           "committed=%d reason=%s map_status=0x%08x",
           failure ? "FAIL" : "PASS", getpid(), move->sourceX, move->sourceY, move->targetX,
           move->targetY, rect->width, rect->height, plan.area, copy.ticket, copy.fence,
           copy.gpuPixels, copy.screenPixels, copy.hash, copy.elapsedMs, copy.committed,
           failure ? failure : "verified", mapStatus);
    return copy.committed ? 0 : (int)kBlitNoResources;
}
#else
static int readArm(XenosWindowArm *arm)
{
    int file = open(XENOS_WINDOW_ARM, kReadOnly | kNonBlocking | kNoFollow);
    if (file < 0)
        return 0;
    unsigned char data[sizeof(*arm) + 1];
    long count = read(file, data, sizeof(data));
    close(file);
    if (count != sizeof(*arm))
        return 0;
    memcpy(arm, data, sizeof(*arm));
    return arm->magic == kGPUWindowCopyMagic;
}

static int blit(void *self, UInt32 options, UInt32 type, UInt32 sourceType,
                const void *operation, const void *source)
{
    Engine *engine = self;
    if (!engine->windowServer || !engine->framebuffer || source ||
        (type != kBlitCopy && type != kBlitCopyRegion))
        return kBlitNoResources;
    XenosWindowArm arm;
    if (!readArm(&arm))
        return kBlitNoResources;
    const char *kind = type == kBlitCopyRegion ? "copy-region" : "copy";
    XenosCopyPlan plan;
    const int decoded = XenosDecodeWindowCopy(type, sourceType, operation, &plan);
    if (!decoded || !XenosWindowCopyCovers(&plan, &arm)) {
        const int traced = traceCount;
        if (traced < 24 && OSAtomicCompareAndSwap32(traced, traced + 1, &traceCount)) {
            if (decoded && plan.count) {
                const XenosCopyRectangle *rect = &plan.rects[0];
                syslog(kNotice, "XenosWinTile skip pid=%d kind=%s options=%u source=0x%x reason=tile-coverage "
                       "rects=%u area=%u first=%d,%d,%d,%d to=%d,%d arm=%d,%d:%d,%d",
                       getpid(), kind, options, sourceType, plan.count, plan.area,
                       rect->move.sourceX, rect->move.sourceY, rect->width, rect->height,
                       rect->move.targetX, rect->move.targetY, arm.move.sourceX, arm.move.sourceY,
                       arm.move.targetX, arm.move.targetY);
            } else {
                syslog(kNotice, "XenosWinTile skip pid=%d kind=%s options=%u source=0x%x reason=decode rects=%u",
                       getpid(), kind, options, sourceType, plan.count);
            }
        }
        return kBlitNoResources;
    }
    /* The long, synchronous diagnostic runs at most once per WindowServer. */
    if (plan.area <= 2 * kWindowCopyPixels || plan.area > kFullCopyPixelLimit ||
        !OSSpinLockTry(&sampleLock))
        return kBlitNoResources;
    int committed = 0;
    if (!attempted) {
        attempted = 1;
        XenosFramebufferMap map;
        XenosFullCopy copy = {.batches = 0};
        UInt32 mapStatus = 0;
        const char *failure = XenosMapWindowAperture(engine->io, engine->framebuffer, &map, &mapStatus);
        if (!failure) {
            const XenosWindowPixels screen = {(volatile UInt32 *)map.address, kFramebufferRowBytes,
                                               kCommonWidth, kCommonHeight};
            failure = XenosCompositeWindowCopy(&screen, &plan, &arm, &copy);
            committed = copy.committed;
            XenosUnmapFramebuffer(&map);
        }
        char fenceList[kBatchFenceTextBytes];
        XenosFormatBatchFences(&copy, fenceList);
        syslog(kNotice, "XenosWinTile %s pid=%d kind=%s src=%d,%d dst=%d,%d hash=%08x "
               "ticket=%u fence=%u batches=%u first_fence=%u gpu_pixels=%u screen_pixels=%u "
               "rects=%u area=%u region_hash=%08x gpu_ms=%u fences=%s reason=%s map_status=0x%08x",
               failure ? "FAIL" : "PASS", getpid(), kind,
               arm.move.sourceX, arm.move.sourceY, arm.move.targetX, arm.move.targetY, arm.hash,
               copy.ticket, copy.fence, copy.batches, copy.firstFence, copy.gpuPixels,
               copy.screenPixels, plan.count, plan.area, copy.hash, copy.elapsedMs, fenceList,
               failure ? failure : "verified", mapStatus);
    }
    OSSpinLockUnlock(&sampleLock);
    return committed ? 0 : (int)kBlitNoResources;
}
#endif

static int getBlitter(void *self, UInt32 options, UInt32 type, UInt32 source, TigerBlitter *out)
{
    (void)self;
    (void)options;
    if (!out)
        return kBlitBadArgument;
    *out = 0;
    source &= ~kBlitDestinationBit;
    if ((type == kBlitFill && source == kBlitSolid) ||
        (type == kBlitCopy && (!source || source == kBlitFramebuffer)) ||
        (type == kBlitCopyRegion && source == kBlitFramebuffer)) {
        *out = blit;
        return 0;
    }
    return kBlitUnsupported;
}

static const TigerBlitInterface interface = {
    .query = query, .retain = retain, .release = release,
    .version = 1, .revision = 2,
    .probe = probe, .start = start, .stop = stop,
    .reset = idle, .capabilities = capabilities, .flush = idle,
    .synchronize = synchronize, .getBeamPosition = beamPosition,
    .allocateSurface = allocateSurface, .freeSurface = unsupportedSurface,
    .lockSurface = surfaceOutput, .unlockSurface = surfaceOutput, .swapSurface = surfaceOutput,
    .setDestination = destination, .getBlitter = getBlitter, .waitComplete = idle,
};

void *XENOS_BLIT_ENTRY(CFRef allocator, CFRef type)
{
    (void)allocator;
    const int server = isWindowServer();
    const char *control = getenv("XENOS_LEGACY_BLIT_CONTROL");
    if (!type || (!server && (!control || strcmp(control, "1"))))
        return 0;
    Engine *engine = calloc(1, sizeof(*engine));
    if (!engine)
        return 0;
    engine->cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation", kDlopenNow);
    engine->io = dlopen("/System/Library/Frameworks/IOKit.framework/Versions/A/IOKit", kDlopenNow);
    if (!engine->cf || !engine->io)
        goto fail;
    TigerUUID (*uuidBytes)(CFRef) = dlsym(engine->cf, "CFUUIDGetUUIDBytes");
    CFRef (*string)(CFRef, const char *, UInt32) = dlsym(engine->cf, "CFStringCreateWithCString");
    CFRef (*uuid)(CFRef, CFRef) = dlsym(engine->cf, "CFUUIDCreateFromString");
    void (*addInstance)(CFRef) = dlsym(engine->cf, "CFPlugInAddInstanceForFactory");
    if (!uuidBytes || !string || !uuid || !addInstance)
        goto fail;
    TigerUUID passedType = uuidBytes(type);
    if (memcmp(&passedType, &graphicsType, sizeof(passedType)))
        goto fail;
#define LOAD(lib, field, symbol) do { \
    engine->field = dlsym(engine->lib, #symbol); \
    if (!engine->field) goto fail; \
} while (0)
    LOAD(cf, releaseCF, CFRelease);
    LOAD(cf, removeInstance, CFPlugInRemoveInstanceForFactory);
    LOAD(io, conforms, IOObjectConformsTo);
    LOAD(io, retainObject, IOObjectRetain);
    LOAD(io, releaseObject, IOObjectRelease);
    LOAD(io, findAccelerator, IOAccelFindAccelerator);
#undef LOAD
    CFRef factoryName = string(0, XENOS_BLIT_FACTORY, 0x600);
    if (!factoryName)
        goto fail;
    engine->factory = uuid(0, factoryName);
    engine->releaseCF(factoryName);
    if (!engine->factory)
        goto fail;
    addInstance(engine->factory);
    engine->interface = &interface;
    engine->refs = 1;
    engine->windowServer = server;
    return engine;

fail:
    if (engine->io)
        dlclose(engine->io);
    if (engine->cf)
        dlclose(engine->cf);
    free(engine);
    return 0;
}
