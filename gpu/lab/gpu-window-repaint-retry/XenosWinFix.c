#include "XenosWindowCopy.h"

extern int _NSGetExecutablePath(char *, UInt32 *);
extern char *realpath(const char *, char *);
extern int getpid(void);
extern int open(const char *, int, ...);
extern int close(int);
extern long read(int, void *, unsigned long);
extern int memcmp(const void *, const void *, unsigned long);
extern void syslog(int, const char *, ...);
extern _Bool OSAtomicCompareAndSwap32(int, int, volatile int *);
extern int OSSpinLockTry(volatile int *);
extern void OSSpinLockUnlock(volatile int *);

enum { kNotice = 5, kReadOnly = 0, kNonBlocking = 4, kNoFollow = 0x100 };

typedef struct {
    const TigerBlitInterface *interface;
    UInt32 refs;
    CFRef factory;
    void *cf, *io;
    io_service_t framebuffer;
    int windowServer;
    volatile int beamReported;
    int (*conforms)(io_object_t, const char *);
    int (*retainObject)(io_object_t);
    int (*releaseObject)(io_object_t);
    int (*findAccelerator)(io_service_t, io_service_t *, UInt32 *);
    void (*removeInstance)(CFRef);
    void (*releaseCF)(CFRef);
} Engine;

static const TigerBlitInterface interface;
static volatile int sampleLock;
static int sampled;
static volatile int traceCount;

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
    if (engine->windowServer)
        syslog(kNotice, "XenosWinFix start pid=%d framebuffer_index=%u", getpid(), index);
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
        syslog(kNotice, "XenosWinFix beam pid=%d status=0x%08x", getpid(), (UInt32)kBlitUnsupported);
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
                syslog(kNotice, "XenosWinFix skip pid=%d kind=%s options=%u source=0x%x reason=tile-coverage "
                       "rects=%u area=%u first=%d,%d,%d,%d to=%d,%d arm=%d,%d:%d,%d",
                       getpid(), kind, options, sourceType, plan.count, plan.area,
                       rect->move.sourceX, rect->move.sourceY, rect->width, rect->height,
                       rect->move.targetX, rect->move.targetY, arm.move.sourceX, arm.move.sourceY,
                       arm.move.targetX, arm.move.targetY);
            } else {
                syslog(kNotice, "XenosWinFix skip pid=%d kind=%s options=%u source=0x%x reason=decode rects=%u",
                       getpid(), kind, options, sourceType, plan.count);
            }
        }
        return kBlitNoResources;
    }
    if (!OSSpinLockTry(&sampleLock))
        return kBlitNoResources;
    int committed = 0;
    if (!sampled) {
        XenosFramebufferMap map;
        XenosFence fence = {.ticket = 0};
        UInt32 copied = 0;
        const char *failure = "framebuffer-map";
        if (XenosMapFramebuffer(engine->io, engine->framebuffer, &map)) {
            const XenosWindowPixels screen = {(volatile UInt32 *)map.address, kFramebufferRowBytes,
                                               kCommonWidth, kCommonHeight};
            failure = XenosCompositeWindowCopy(&screen, &plan, &arm, &fence, &copied, &committed);
            XenosUnmapFramebuffer(&map);
        }
        if (committed)
            sampled = 1;
        syslog(kNotice, "XenosWinFix %s pid=%d kind=%s src=%d,%d dst=%d,%d hash=%08x "
               "ticket=%u fence=%u pixels=%u rects=%u area=%u reason=%s",
               failure ? "FAIL" : "PASS", getpid(), kind,
               arm.move.sourceX, arm.move.sourceY, arm.move.targetX, arm.move.targetY, arm.hash,
               fence.ticket, fence.fence, copied, plan.count, plan.area, failure ? failure : "verified");
    }
    OSSpinLockUnlock(&sampleLock);
    return committed ? 0 : (int)kBlitNoResources;
}

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

void *XenosWinFixFactory(CFRef allocator, CFRef type)
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
