#include "XenosBlitABI.h"
#include "Offscreen.h"

extern unsigned long strlen(const char *);
extern int getpid(void);

typedef struct {
    CFRef (*string)(CFRef, const char *, UInt32);
    CFRef (*url)(CFRef, const unsigned char *, long, unsigned char);
    CFRef (*uuid)(CFRef, CFRef);
    CFRef (*plugin)(CFRef, CFRef);
    void *(*instance)(CFRef, CFRef, CFRef);
    CFRef (*property)(io_object_t, CFRef, CFRef, UInt32);
    void (*release)(CFRef);
    unsigned long (*type)(CFRef);
    unsigned long (*dataType)(void);
    long (*dataLength)(CFRef);
    const unsigned char *(*dataBytes)(CFRef);
    int (*beamPosition)(void *, UInt32, int *);
    int (*synchronize)(void *, UInt32 x, UInt32 y, UInt32 width, UInt32 height, UInt32 options);
} API;

static int completion(const API *api, io_service_t gpu, XenosGPUCompletion *out)
{
    CFRef key = api->string(0, XENOS_GPU_COMPLETION_KEY, 0x600);
    if (!key)
        return 0;
    CFRef data = api->property(gpu, key, 0, 0);
    api->release(key);
    if (!data)
        return 0;
    const int valid = api->type(data) == api->dataType() && api->dataLength(data) == sizeof(*out);
    if (valid)
        memcpy(out, api->dataBytes(data), sizeof(*out));
    api->release(data);
    return valid;
}

static int testTiming(const API *api, const TigerBlitInterface **blit, io_service_t gpu)
{
    if (!(*blit)->getBeamPosition || !(*blit)->synchronize)
        return 0;
    /* These two Tiger wrappers use only the IOPS engine's first field. */
    struct { const TigerBlitInterface **interface; } engine = {blit};
    XenosGPUCompletion before = {.sequence = 0}, after = {.sequence = 0};
    const int haveBefore = completion(api, gpu, &before);
    const int untouched = 0x12345678;
    int position = untouched;
    UInt32 address = 0x11223344, flags = 0x55667788;
    const UInt32 beamStatus = api->beamPosition(&engine, 0, &position);
    const UInt32 nullStatus = api->beamPosition(&engine, 0, 0);
    const UInt32 syncStatus = api->synchronize(&engine, 2, 3, 24, 19, 0);
    const TigerBlitInterface *methods = *blit;
    const int surfacesSafe = methods->allocateSurface && methods->freeSurface && methods->lockSurface &&
        methods->unlockSurface && methods->swapSurface &&
        (UInt32)methods->allocateSurface(blit, 0, 0, 0) == kBlitUnsupported &&
        (UInt32)methods->freeSurface(blit, 0, 0) == kBlitUnsupported &&
        (UInt32)methods->lockSurface(blit, 0, 0, &address) == kBlitUnsupported &&
        (UInt32)methods->unlockSurface(blit, 0, 0, &flags) == kBlitUnsupported &&
        (UInt32)methods->swapSurface(blit, 0, 0, &flags) == kBlitUnsupported &&
        address == 0x11223344 && flags == 0x55667788;
    const int noFence = haveBefore && completion(api, gpu, &after) && after.sequence == before.sequence;
    const int ok = beamStatus == kBlitUnsupported && nullStatus == kBlitBadArgument &&
        syncStatus == kBlitUnsupported && position == untouched && surfacesSafe && noFence;
    printf("XenosFramebufferBlit timing %s IOFBBeamPosition=0x%08x null=0x%08x "
           "IOFBSynchronize=0x%08x unchanged=%d surfaces_safe=%d no_fence=%d\n",
           ok ? "PASS" : "FAIL", beamStatus, nullStatus, syncStatus, position == untouched,
           surfacesSafe, noFence);
    return ok;
}

static int testControl(const API *api, io_service_t framebuffer, io_service_t gpu, const char *bundle)
{
    CFRef url = api->url(0, (const unsigned char *)bundle, strlen(bundle), 1);
    if (!url)
        return 0;
    CFRef plugin = api->plugin(0, url);
    api->release(url);
    if (!plugin)
        return 0;
    CFRef typeName = api->string(0, "ACCF0000-0000-0000-0000-000A2789904E", 0x600);
    CFRef factoryName = api->string(0, XENOS_BLIT_FACTORY, 0x600);
    CFRef type = typeName ? api->uuid(0, typeName) : 0;
    CFRef factory = factoryName ? api->uuid(0, factoryName) : 0;
    const TigerBlitInterface **base = type && factory ? api->instance(0, factory, type) : 0;
    if (typeName) api->release(typeName);
    if (factoryName) api->release(factoryName);
    if (type) api->release(type);
    if (factory) api->release(factory);

    const TigerBlitInterface **blit = 0;
    if (base) {
        (*base)->query(base, graphicsInterface, (void **)&blit);
        (*base)->release(base);
    }
    if (!blit) {
        api->release(plugin);
        printf("XenosFramebufferBlit FAIL CFPlugIn factory or interface\n");
        return 0;
    }
    int score = 0;
    int status = (*blit)->probe(blit, 0, framebuffer, &score);
    if (!status)
        status = (*blit)->start(blit, 0, framebuffer);
    TigerBlitter copy = 0, fill = 0, render = 0;
    if (!status)
        status = (*blit)->setDestination(blit, 0, 0);
    if (!status)
        status = (*blit)->getBlitter(blit, ~0U, kBlitCopy, 0, &copy);
    if (!status)
        status = (*blit)->getBlitter(blit, ~0U, kBlitFill, kBlitSolid, &fill);
    if (!status)
        status = (*blit)->getBlitter(blit, ~0U, kBlitCopyRegion, kBlitFramebuffer, &render);
    printf("XenosFramebufferBlit interface status=0x%08x score=%d copy=%d fill=%d region=%d\n",
           (UInt32)status, score, copy != 0, fill != 0, render != 0);

    int ok = 0;
    if (!status && copy && fill && render && testTiming(api, blit, gpu)) {
        XenosGPUCompletion before = {.sequence = 0}, after = {.sequence = 0}, repeat = {.sequence = 0};
        completion(api, gpu, &before);
        const TigerFill invalid = {.count = 0};
        const TigerFill repaint = {.count = 1, .rectangle = {2, 3, 24, 19}};
        int reject = fill(blit, 0, kBlitFill, kBlitSolid | kBlitDestinationBit, &invalid, 0);
        int rejectedByGPU = completion(api, gpu, &repeat) && repeat.sequence == before.sequence;
        int fallback = fill(blit, 0, kBlitFill, kBlitSolid | kBlitDestinationBit, &repaint, 0);
        const int fresh = completion(api, gpu, &after) && after.sequence && after.sequence != before.sequence &&
            after.ownerPID == (UInt32)getpid() && after.ticket && after.ticket == after.fence &&
            after.state == kXenosQuadPassed && after.pixels == kXenosQuadReadbackPixels && !after.mismatches;
        int repeatFallback = fill(blit, 0, kBlitFill, kBlitSolid | kBlitDestinationBit, &repaint, 0);
        const int once = completion(api, gpu, &repeat) && repeat.sequence == after.sequence;
        ok = fresh && once && rejectedByGPU && (UInt32)reject == kBlitNoResources &&
            (UInt32)fallback == kBlitNoResources && (UInt32)repeatFallback == kBlitNoResources;
        printf("XenosFramebufferBlit %s owner=%u pid=%d ticket=%u fence=%u pixels=%u "
               "mismatches=%u fallback=0x%08x one_shot=%d invalid_ignored=%d\n",
               ok ? "PASS" : "FAIL", after.ownerPID, getpid(), after.ticket, after.fence,
               after.pixels, after.mismatches, (UInt32)fallback, once, rejectedByGPU);
    }
    (*blit)->stop(blit);
    (*blit)->release(blit);
    api->release(plugin);
    return ok;
}

int main(int argc, char **argv)
{
    if ((argc == 2 && strcmp(argv[1], "--lookup")) || (argc == 3 && strcmp(argv[1], "--control")) ||
        (argc != 2 && argc != 3))
        return 2;
    void *cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation", kDlopenNow);
    void *io = dlopen("/System/Library/Frameworks/IOKit.framework/Versions/A/IOKit", kDlopenNow);
    if (!cf || !io)
        return 1;
    API api;
#define LOAD(lib, field, symbol) do { \
    api.field = dlsym(lib, #symbol); \
    if (!api.field) return 1; \
} while (0)
    LOAD(cf, string, CFStringCreateWithCString);
    LOAD(cf, url, CFURLCreateFromFileSystemRepresentation);
    LOAD(cf, uuid, CFUUIDCreateFromString);
    LOAD(cf, plugin, CFPlugInCreate);
    LOAD(cf, instance, CFPlugInInstanceCreate);
    LOAD(cf, type, CFGetTypeID);
    LOAD(cf, dataType, CFDataGetTypeID);
    LOAD(cf, dataLength, CFDataGetLength);
    LOAD(cf, dataBytes, CFDataGetBytePtr);
    LOAD(cf, release, CFRelease);
    LOAD(io, property, IORegistryEntryCreateCFProperty);
    LOAD(io, beamPosition, IOFBBeamPosition);
    LOAD(io, synchronize, IOFBSynchronize);
#undef LOAD
    void *(*match)(const char *) = dlsym(io, "IOServiceMatching");
    io_service_t (*find)(UInt32, void *) = dlsym(io, "IOServiceGetMatchingService");
    int (*drop)(io_object_t) = dlsym(io, "IOObjectRelease");
    int (*allocate)(io_service_t, void **, int *) = dlsym(io, "IOPSAllocateBlitEngine");
    void (*deallocate)(void *) = dlsym(io, "IOPSBlitDeallocate");
    if (!match || !find || !drop || !allocate || !deallocate)
        return 1;
    io_service_t fb = find(0, match("XenonFramebufferTest"));
    io_service_t gpu = find(0, match("XenosController"));
    int success = 0;
    if (fb && argc == 2) {
        void *engine = 0;
        int score = 0;
        int status = allocate(fb, &engine, &score);
        printf("XenosFramebufferBlit IOPSAllocateBlitEngine=0x%08x engine=%d score=%d\n",
               (UInt32)status, engine != 0, score);
        success = !status && engine;
        if (success)
            deallocate(engine);
    } else if (fb && gpu) {
        success = testControl(&api, fb, gpu, argv[2]);
    }
    if (gpu) drop(gpu);
    if (fb) drop(fb);
    return !success;
}
