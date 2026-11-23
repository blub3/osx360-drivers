#include "Offscreen.h"

extern int getpid(void);
#include "XenosDisplaySurfaceProtocol.h"

typedef const void *CFRef;

typedef struct {
    void *io;
    void *cf;
    void *(*matching)(const char *);
    io_service_t (*find)(UInt32, void *);
    CFRef (*property)(io_object_t, CFRef, CFRef, UInt32);
    int (*releaseObject)(io_object_t);
    CFRef (*string)(CFRef, const char *, UInt32);
    unsigned long (*type)(CFRef);
    unsigned long (*dataType)(void);
    long (*dataLength)(CFRef);
    const unsigned char *(*dataBytes)(CFRef);
    void (*releaseCF)(CFRef);
} Registry;

static int openRegistry(Registry *api)
{
    api->io = dlopen("/System/Library/Frameworks/IOKit.framework/Versions/A/IOKit", kDlopenNow);
    api->cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation", kDlopenNow);
    if (!api->io || !api->cf)
        return 0;
#define LOAD(lib, field, symbol) do { \
    api->field = dlsym(api->lib, #symbol); \
    if (!api->field) return 0; \
} while (0)
    LOAD(io, matching, IOServiceMatching);
    LOAD(io, find, IOServiceGetMatchingService);
    LOAD(io, property, IORegistryEntryCreateCFProperty);
    LOAD(io, releaseObject, IOObjectRelease);
    LOAD(cf, string, CFStringCreateWithCString);
    LOAD(cf, type, CFGetTypeID);
    LOAD(cf, dataType, CFDataGetTypeID);
    LOAD(cf, dataLength, CFDataGetLength);
    LOAD(cf, dataBytes, CFDataGetBytePtr);
    LOAD(cf, releaseCF, CFRelease);
#undef LOAD
    return 1;
}

static int snapshot(const Registry *api, io_service_t gpu, XenosGPUCompletion *out)
{
    enum { kCFStringASCII = 0x0600 };
    CFRef key = api->string(0, XENOS_GPU_COMPLETION_KEY, kCFStringASCII);
    if (!key)
        return 0;
    CFRef data = api->property(gpu, key, 0, 0);
    api->releaseCF(key);
    if (!data)
        return 0;
    int valid = api->type(data) == api->dataType() && api->dataLength(data) == sizeof(*out);
    if (valid)
        memcpy(out, api->dataBytes(data), sizeof(*out));
    api->releaseCF(data);
    return valid;
}

typedef struct {
    unsigned count;
    short x, y, w, h;
} TigerDeviceRegion;
typedef char TigerDeviceRegionSize[sizeof(TigerDeviceRegion) == 12 ? 1 : -1];
typedef char TigerSurfaceInfoSize[sizeof(XenosTigerSurfaceInfo) == 68 ? 1 : -1];

enum {
    kSurfaceModeARGBWindowed = 0x24,
    kBusy = 0xe00002d5U,
    kNotReady = 0xe00002d8U,
    kUnsupported = 0xe00002c7U,
    kBadArgument = 0xe00002c2U,
};

typedef struct {
    int (*findAccelerator)(io_service_t, io_service_t *, UInt32 *);
    int (*conforms)(io_object_t, const char *);
    int (*parent)(io_object_t, const char *, io_object_t *);
    int (*create)(io_service_t, UInt32, UInt32, UInt32 *);
    int (*setShape)(UInt32, const TigerDeviceRegion *, UInt32, UInt32);
    int (*flush)(UInt32, UInt32, UInt32);
    int (*readLock)(UInt32, XenosTigerSurfaceInfo *, UInt32);
    int (*readUnlock)(UInt32);
    int (*destroy)(UInt32);
} SurfaceAPI;

static int loadSurfaceAPI(void *io, SurfaceAPI *api)
{
#define LOAD(field, symbol) do { \
    api->field = dlsym(io, #symbol); \
    if (!api->field) { printf("Missing %s\n", #symbol); return 0; } \
} while (0)
    LOAD(findAccelerator, IOAccelFindAccelerator);
    LOAD(conforms, IOObjectConformsTo);
    LOAD(parent, IORegistryEntryGetParentEntry);
    LOAD(create, IOAccelCreateSurface);
    LOAD(setShape, IOAccelSetSurfaceFramebufferShape);
    LOAD(flush, IOAccelFlushSurfaceOnFramebuffers);
    LOAD(readLock, IOAccelReadLockSurface);
    LOAD(readUnlock, IOAccelReadUnlockSurface);
    LOAD(destroy, IOAccelDestroySurface);
#undef LOAD
    return 1;
}

static int locateAccelerator(const Registry *reg, const SurfaceAPI *api, CGDirectDisplayID display,
                             io_service_t *accelerator, UInt32 *index)
{
    void *cg = dlopen("/System/Library/Frameworks/ApplicationServices.framework/Versions/A/"
                      "Frameworks/CoreGraphics.framework/Versions/A/CoreGraphics", kDlopenNow);
    if (!cg)
        return 0;
    io_service_t (*servicePort)(CGDirectDisplayID) = dlsym(cg, "CGDisplayIOServicePort");
    const io_service_t borrowed = servicePort ? servicePort(display) : 0;
    io_service_t node = borrowed;
    int result = 0;
    while (node) {
        if (api->conforms(node, "XenonFramebufferTest")) {
            int status = api->findAccelerator(node, accelerator, index);
            printf("XenosDisplaySurface IOAccelFindAccelerator=0x%08x index=%u\n", (UInt32)status, *index);
            result = !status && *accelerator && api->conforms(*accelerator, "XenosDisplayAccelerator");
            if (node != borrowed)
                reg->releaseObject(node);
            break;
        }
        io_service_t parent = 0;
        api->parent(node, "IOService", &parent);
        if (node != borrowed)
            reg->releaseObject(node);
        node = parent;
    }
    dlclose(cg);
    return result;
}

static int fail(const char *operation, int status)
{
    printf("XenosDisplaySurface FAIL %s status=0x%08x\n", operation, (UInt32)status);
    return 0;
}

static int checkClear(const Registry *reg, const SurfaceAPI *api, io_service_t gpu,
                      UInt32 connection, UInt32 index, UInt32 expected)
{
    XenosGPUCompletion before = {.sequence = 0}, after = {.sequence = 0};
    snapshot(reg, gpu, &before);

    int status = (int)kBusy;
    for (unsigned i = 0; i < 250 && (UInt32)status == kBusy; ++i) {
        status = api->flush(connection, 0, 1U << index);
        if ((UInt32)status == kBusy)
            usleep(10000);
    }
    if (status)
        return fail("IOAccelFlushSurfaceOnFramebuffers", status);

    XenosTigerSurfaceInfo info;
    status = (int)kNotReady;
    for (unsigned i = 0; i < 250 && (UInt32)status == kNotReady; ++i) {
        status = api->readLock(connection, &info, sizeof(info));
        if ((UInt32)status == kNotReady)
            usleep(10000);
    }
    if (status)
        return fail("IOAccelReadLockSurface", status);

    const int layout = info.address[0] && !(info.address[0] & 4095U) && !info.address[1] &&
                       !info.address[2] && !info.address[3] && info.width == kXenosSurfaceSide &&
                       info.height == kXenosSurfaceSide && info.rowBytes == kXenosSurfaceSide*sizeof(UInt32) &&
                       info.pixelFormat == kXenosDiagnosticSurfaceFormat;
    UInt32 checked = 0, mismatches = 0, firstPixel = 0;
    if (layout) {
        const UInt32 *pixels = (const UInt32 *)info.address[0];
        firstPixel = pixels[0];
        for (UInt32 i = 0; i < kXenosSurfaceSide*kXenosSurfaceSide; ++i) {
            mismatches += pixels[i] != expected;
            ++checked;
        }
    }
    int blocked = api->flush(connection, 0, 1U << index);
    status = api->readUnlock(connection);
    if (status || (UInt32)blocked != kBusy)
        return fail("IOAccel read lock exclusion and unlock", status ? status : blocked);

    const int trace = snapshot(reg, gpu, &after) && after.sequence != before.sequence &&
                      after.sequence && after.ownerPID == (UInt32)getpid() &&
                      after.ticket && after.fence == after.ticket && after.state == kXenosQuadPassed &&
                      after.pixels == kXenosSurfaceSide*kXenosSurfaceSide && !after.mismatches;
    printf("XenosDisplaySurface %s seq=%u pid=%d owner_pid=%u ticket=%u fence=%u "
           "pixels=%u mismatches=%u argb=%08x expected=%08x stride=%u read_lock=verified\n",
           trace && layout && !mismatches ? "PASS" : "FAIL", after.sequence, getpid(), after.ownerPID,
           after.ticket, after.fence, checked, mismatches, firstPixel, expected, info.rowBytes);
    return trace && layout && !mismatches;
}

static int run(const Registry *reg, const SurfaceAPI *api, io_service_t gpu, io_service_t accelerator,
               UInt32 index, int baseline)
{
    UInt32 surface = 0;
    int status = api->create(accelerator, kXenosDiagnosticSurfaceID, kSurfaceModeARGBWindowed, &surface);
    if (baseline) {
        if (surface)
            api->destroy(surface);
        if ((UInt32)status != kUnsupported || surface)
            return fail("baseline IOAccelCreateSurface", status);
        printf("XenosDisplaySurface PASS baseline IOAccelCreateSurface=0x%08x (surface client unavailable)\n",
               (UInt32)status);
        return 1;
    }
    if (status || !surface)
        return fail("IOAccelCreateSurface", status);

    TigerDeviceRegion bad = {0, 0, 0, kXenosSurfaceSide - 1, kXenosSurfaceSide};
    const TigerDeviceRegion shape = {0, 0, 0, kXenosSurfaceSide, kXenosSurfaceSide};
    int ok = 0;
    status = api->setShape(surface, &bad, 0, index);
    if ((UInt32)status != kBadArgument)
        fail("unsupported surface shape", status);
    else if ((status = api->setShape(surface, &shape, 0, index)) != 0)
        fail("IOAccelSetSurfaceFramebufferShape", status);
    else
        ok = checkClear(reg, api, gpu, surface, index, kXenosDiagnosticSurfaceFirst) &&
             checkClear(reg, api, gpu, surface, index, kXenosDiagnosticSurfaceSecond);

    status = api->destroy(surface);
    if (status)
        return fail("IOAccelDestroySurface", status);
    if (ok)
        printf("XenosDisplaySurface PASS create shape flush readlock unlock destroy\n");
    return ok;
}

int main(int argc, char **argv)
{
    if (argc != 2 || (strcmp(argv[1], "--baseline") && strcmp(argv[1], "--probe")))
        return 2;
    XenosDisplay display;
    Registry reg = {.io = 0};
    SurfaceAPI api;
    if (!XenosFindDisplay(&display) || !openRegistry(&reg) || !loadSurfaceAPI(reg.io, &api)) {
        fail("open active Xbox display and Tiger IOKit", -1);
        return 1;
    }

    io_service_t accelerator = 0;
    const io_service_t gpu = reg.find(0, reg.matching("XenosController"));
    UInt32 index = 0;
    int success = gpu && locateAccelerator(&reg, &api, display.display, &accelerator, &index) &&
                  index == 0 && run(&reg, &api, gpu, accelerator, index, !strcmp(argv[1], "--baseline"));
    if (accelerator)
        reg.releaseObject(accelerator);
    if (gpu)
        reg.releaseObject(gpu);
    dlclose(reg.cf);
    dlclose(reg.io);
    return !success;
}
