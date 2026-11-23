#include "Offscreen.h"

extern int getpid(void);
extern int setenv(const char *, const char *, int);
extern void unsetenv(const char *);

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

static int query(UInt32 mask, int expectXenos)
{
    void *openGL = dlopen("/System/Library/Frameworks/OpenGL.framework/Versions/A/OpenGL",
                          kDlopenNow);
    if (!openGL)
        return 0;
    int (*queryRenderers)(UInt32, CGLRendererInfoObj *, int *) = dlsym(openGL, "CGLQueryRendererInfo");
    int (*describe)(CGLRendererInfoObj, int, int, int *) = dlsym(openGL, "CGLDescribeRenderer");
    int (*destroy)(CGLRendererInfoObj) = dlsym(openGL, "CGLDestroyRendererInfo");
    int success = 0;
    CGLRendererInfoObj info = 0;
    int count = 0;
    if (!queryRenderers || !describe || !destroy)
        goto done;
    int status = queryRenderers(mask, &info, &count);
    printf("XenosNativeGLD CGLQueryRendererInfo=%d count=%d mask=0x%08x\n", status, count, mask);
    if (status || !info || count <= 0)
        goto done;

    int matches = 0, stock = 0;
    for (int i = 0; i < count; ++i) {
        int id, accelerated, rendererMask;
        if (describe(info, i, kCGLRPRendererID, &id) ||
            describe(info, i, kCGLRPAccelerated, &accelerated) ||
            describe(info, i, kCGLRPDisplayMask, &rendererMask))
            goto done;
        printf("renderer id=0x%08x accelerated=%d displayMask=0x%08x\n",
               (UInt32)id, accelerated, (UInt32)rendererMask);
        if (id == kXenosCGLRendererID) {
            if (accelerated || (UInt32)rendererMask != mask)
                goto done;
            ++matches;
        } else {
            ++stock;
        }
    }
    success = matches == expectXenos && stock > 0;
done:
    if (info && destroy(info))
        success = 0;
    dlclose(openGL);
    return success;
}

int main(int argc, char **argv)
{
    if (argc != 2 || (strcmp(argv[1], "--baseline") && strcmp(argv[1], "--probe")))
        return 2;
    const int probe = !strcmp(argv[1], "--probe");
    XenosDisplay display;
    Registry api = {.io = 0};
    unsetenv("XENOS_NATIVE_GLD_PROBE");
    if (!XenosFindDisplay(&display) || !openRegistry(&api)) {
        printf("XenosNativeGLD FAIL opening active Xbox display and IOKit\n");
        return 1;
    }
    io_service_t gpu = api.find(0, api.matching("XenosController"));
    if (!gpu)
        return 1;

    XenosGPUCompletion before = {.sequence = 0}, after = {.sequence = 0};
    snapshot(&api, gpu, &before);
    int found = (!probe || !setenv("XENOS_NATIVE_GLD_PROBE", "1", 1)) && query(display.mask, probe);
    unsetenv("XENOS_NATIVE_GLD_PROBE");
    int haveCompletion = snapshot(&api, gpu, &after);
    api.releaseObject(gpu);
    dlclose(api.cf);
    dlclose(api.io);
    int verified = haveCompletion && after.sequence != before.sequence && after.sequence &&
                   after.ownerPID == (UInt32)getpid() && after.ticket && after.fence == after.ticket &&
                   after.state == kXenosQuadPassed && after.pixels == kXenosQuadReadbackPixels &&
                   !after.mismatches;
    printf("XenosNativeGLD registry sequence_before=%u sequence_after=%u pid=%d owner_pid=%u "
           "ticket=%u fence=%u state=%u pixels=%u mismatches=%u\n",
           before.sequence, after.sequence, getpid(), after.ownerPID, after.ticket, after.fence,
           after.state, after.pixels, after.mismatches);
    if (!found || (probe ? !verified : after.sequence != before.sequence)) {
        printf("XenosNativeGLD FAIL mode=%s discovery=%d owner_trace=%d\n",
               probe ? "probe" : "baseline", found, verified);
        return 1;
    }
    printf("XenosNativeGLD PASS mode=%s\n", probe ? "GLD fence" : "baseline");
    return 0;
}
