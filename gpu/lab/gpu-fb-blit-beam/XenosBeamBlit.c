#include "XenosBlitABI.h"
#include "Offscreen.h"
#include "XenosDisplaySurfaceProtocol.h"

extern int _NSGetExecutablePath(char *, UInt32 *);
extern char *realpath(const char *, char *);
extern int getpid(void);
extern int access(const char *, int);
extern int memcmp(const void *, const void *, unsigned long);
extern void syslog(int, const char *, ...);
extern _Bool OSAtomicCompareAndSwap32(int, int, volatile int *);
extern int OSSpinLockTry(volatile int *);
extern void OSSpinLockUnlock(volatile int *);

enum { kNotice = 5, kReadAccess = 4, kBusy = 0xe00002d5U, kNotReady = 0xe00002d8U };

typedef struct {
    const TigerBlitInterface *interface;
    UInt32 refs;
    CFRef factory;
    void *cf, *io;
    io_service_t accelerator;
    int windowServer;
    volatile int beamReported;
    int (*conforms)(io_object_t, const char *);
    int (*releaseObject)(io_object_t);
    int (*findAccelerator)(io_service_t, io_service_t *, UInt32 *);
    int (*create)(io_service_t, UInt32, UInt32, UInt32 *);
    int (*shape)(UInt32, const void *, UInt32, UInt32);
    int (*flush)(UInt32, UInt32, UInt32);
    int (*readLock)(UInt32, XenosTigerSurfaceInfo *, UInt32);
    int (*readUnlock)(UInt32);
    int (*destroy)(UInt32);
    void (*removeInstance)(CFRef);
    void (*releaseCF)(CFRef);
} Engine;

static const TigerBlitInterface interface;
static volatile int sampleLock;
static int sampled;

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
        if (engine->accelerator)
            engine->releaseObject(engine->accelerator);
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
    if (engine->accelerator || probe(self, properties, framebuffer, &score))
        return kBlitUnsupported;
    int status = engine->findAccelerator(framebuffer, &accelerator, &index);
    if (status || !accelerator || index || !engine->conforms(accelerator, "XenosDisplayAccelerator")) {
        if (accelerator)
            engine->releaseObject(accelerator);
        return status ? status : (int)kBlitUnsupported;
    }
    engine->accelerator = accelerator;
    if (engine->windowServer)
        syslog(kNotice, "XenosBeamBlit start pid=%d framebuffer_index=%u", getpid(), index);
    return 0;
}

static int stop(void *self)
{
    Engine *engine = self;
    if (engine->accelerator)
        engine->releaseObject(engine->accelerator);
    engine->accelerator = 0;
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
        syslog(kNotice, "XenosBeamBlit beam pid=%d status=0x%08x", getpid(), (UInt32)kBlitUnsupported);
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

static int surfaceSample(Engine *engine, UInt32 *checked)
{
    const struct { UInt32 count; short x, y, width, height; } region =
        {0, 0, 0, kXenosSurfaceSide, kXenosSurfaceSide};
    UInt32 connection = 0;
    int status = engine->create(engine->accelerator, kXenosDiagnosticSurfaceID, 0x24, &connection);
    if (status || !connection)
        return status ? status : (int)kBlitNoResources;
    status = engine->shape(connection, &region, 0, 0);
    if (!status) {
        for (unsigned attempt = 0; attempt < 40; ++attempt) {
            status = engine->flush(connection, 0, 1);
            if ((UInt32)status != kBusy)
                break;
            usleep(5000);
        }
    }
    XenosTigerSurfaceInfo info;
    if (!status) {
        for (unsigned attempt = 0; attempt < 40; ++attempt) {
            status = engine->readLock(connection, &info, sizeof(info));
            if ((UInt32)status != kNotReady)
                break;
            usleep(5000);
        }
    }
    if (!status) {
        if (!info.address[0] || (info.address[0] & 4095U) ||
            info.width != kXenosSurfaceSide || info.height != kXenosSurfaceSide ||
            info.rowBytes != kXenosSurfaceSide * sizeof(UInt32) ||
            info.pixelFormat != kXenosDiagnosticSurfaceFormat) {
            status = kBlitBadArgument;
        } else {
            const UInt32 *pixels = (const UInt32 *)info.address[0];
            for (UInt32 i = 0; i < kXenosSurfaceSide*kXenosSurfaceSide; ++i)
                *checked += pixels[i] == kXenosDiagnosticSurfaceFirst;
        }
        int unlock = engine->readUnlock(connection);
        if (!status)
            status = unlock;
    }
    int close = engine->destroy(connection);
    return status ? status : close;
}

static int blit(void *self, UInt32 options, UInt32 type, UInt32 sourceType,
                const void *operation, const void *source)
{
    Engine *engine = self;
    (void)options;
    (void)source;
    sourceType &= ~kBlitDestinationBit;
    if (!engine->accelerator || !operation)
        return kBlitBadArgument;
    const char *name;
    if (type == kBlitFill && sourceType == kBlitSolid) {
        const TigerFill *fill = operation;
        if (fill->count != 1 || fill->rectangle.width <= 0 || fill->rectangle.height <= 0)
            return kBlitNoResources;
        name = "fill";
    } else if (type == kBlitCopy && (!sourceType || sourceType == kBlitFramebuffer)) {
        const TigerCopy *copy = operation;
        if (copy->count != 1 || copy->rectangle.width <= 0 || copy->rectangle.height <= 0)
            return kBlitNoResources;
        name = "copy";
    } else if (type == kBlitCopyRegion && sourceType == kBlitFramebuffer) {
        const TigerCopyRegion *region = operation;
        if (!region->region)
            return kBlitNoResources;
        name = "copy-region";
    } else {
        return kBlitNoResources;
    }

    const char *arm = "/Volumes/OSX360BOOT/XenosFramebufferBeamTest/repaint-blit.arm";
    if ((!engine->windowServer || !access(arm, kReadAccess)) && OSSpinLockTry(&sampleLock)) {
        int run = !sampled;
        sampled = 1;
        OSSpinLockUnlock(&sampleLock);
        if (run) {
            UInt32 checked = 0;
            const int status = surfaceSample(engine, &checked);
            const char *pass = !status && checked == kXenosSurfaceSide*kXenosSurfaceSide ? "PASS" : "FAIL";
            if (engine->windowServer)
                syslog(kNotice, "XenosBeamBlit %s source=%s pid=%d pixels=%u status=0x%08x",
                       pass, name, getpid(), checked, (UInt32)status);
            else
                printf("XenosBeamBlit %s source=%s pid=%d pixels=%u status=0x%08x\n",
                       pass, name, getpid(), checked, (UInt32)status);
        }
    }
    /* No scanout pixels were changed. Tiger retries the original blit in software. */
    return kBlitNoResources;
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

void *XenosBeamBlitFactory(CFRef allocator, CFRef type)
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
    LOAD(io, releaseObject, IOObjectRelease);
    LOAD(io, findAccelerator, IOAccelFindAccelerator);
    LOAD(io, create, IOAccelCreateSurface);
    LOAD(io, shape, IOAccelSetSurfaceFramebufferShape);
    LOAD(io, flush, IOAccelFlushSurfaceOnFramebuffers);
    LOAD(io, readLock, IOAccelReadLockSurface);
    LOAD(io, readUnlock, IOAccelReadUnlockSurface);
    LOAD(io, destroy, IOAccelDestroySurface);
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
