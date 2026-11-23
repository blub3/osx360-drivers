#include "Offscreen.h"
#include "XenosCompositorProtocol.h"

extern int getpid(void);

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

static int checkContext(const Registry *api, io_service_t gpu)
{
    void *openGL = dlopen("/System/Library/Frameworks/OpenGL.framework/Versions/A/OpenGL", kDlopenNow);
    if (!openGL)
        return 0;
    int (*choose)(const int *, CGLPixelFormatObj *, int *) = dlsym(openGL, "CGLChoosePixelFormat");
    int (*describe)(CGLPixelFormatObj, int, int, int *) = dlsym(openGL, "CGLDescribePixelFormat");
    int (*create)(CGLPixelFormatObj, CGLContextObj, CGLContextObj *) = dlsym(openGL, "CGLCreateContext");
    int (*offscreen)(CGLContextObj, int, int, int, void *) = dlsym(openGL, "CGLSetOffScreen");
    int (*current)(CGLContextObj) = dlsym(openGL, "CGLSetCurrentContext");
    int (*destroyContext)(CGLContextObj) = dlsym(openGL, "CGLDestroyContext");
    int (*destroyFormat)(CGLPixelFormatObj) = dlsym(openGL, "CGLDestroyPixelFormat");
    void (*clearColor)(float, float, float, float) = dlsym(openGL, "glClearColor");
    void (*clear)(unsigned) = dlsym(openGL, "glClear");
    void (*flush)(void) = dlsym(openGL, "glFlush");
    void (*viewport)(int, int, int, int) = dlsym(openGL, "glViewport");
    void (*readPixels)(int, int, int, int, unsigned, unsigned, void *) = dlsym(openGL, "glReadPixels");
    unsigned (*getError)(void) = dlsym(openGL, "glGetError");
    if (!choose || !describe || !create || !offscreen || !current || !destroyContext ||
        !destroyFormat || !clearColor || !clear || !flush || !viewport || !readPixels || !getError) {
        dlclose(openGL);
        return 0;
    }

    const int attrs[] = {kCGLPFARendererID, kXenosCompositorRendererID, kOffscreenAttribute, 0};
    CGLPixelFormatObj format = 0;
    CGLContextObj context = 0;
    int count = 0, renderer = 0, ok = 0;
    int status = choose(attrs, &format, &count);
    printf("XenosCompositorContext choose=%d formats=%d\n", status, count);
    fflush(0);
    if (status || !format || !count || describe(format, 0, kCGLPFARendererID, &renderer) ||
        renderer != kXenosCompositorRendererID)
        goto done;
    status = create(format, 0, &context);
    printf("XenosCompositorContext create=%d context=%p renderer=%08x\n", status, (void *)context, (UInt32)renderer);
    fflush(0);
    if (status || !context)
        goto done;
    UInt32 drawables[2][32*32];
    ok = 1;
    for (int i = 0; i < 2; ++i) {
        XenosGPUCompletion before = {.sequence = 0}, after = {.sequence = 0};
        snapshot(api, gpu, &before);
        status = offscreen(context, 32, 32, 32*sizeof(UInt32), drawables[i]);
        printf("XenosCompositorContext attach=%d pass=%d\n", status, i + 1);
        if (status || current(context)) {
            ok = 0;
            break;
        }
        viewport(0, 0, 32, 32);
        clearColor(i == 0, i == 1, 0, 1);
        clear(GL_COLOR_BUFFER_BIT);
        flush();
        unsigned char rgba[4] = {0, 0, 0, 0};
        readPixels(16, 16, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        unsigned glStatus = getError();
        const int color = rgba[0] == (i == 0 ? 255 : 0) && rgba[1] == (i == 1 ? 255 : 0) &&
                          !rgba[2] && rgba[3] == 255;
        const int fence = snapshot(api, gpu, &after) && after.sequence && after.sequence != before.sequence &&
                          after.ownerPID == (UInt32)getpid() && after.ticket && after.ticket == after.fence &&
                          after.state == kXenosQuadPassed && after.pixels == kXenosQuadReadbackPixels && !after.mismatches;
        printf("XenosCompositorContext %s pass=%d pid=%d owner=%u sequence=%u ticket=%u fence=%u "
               "pixels=%u rgba=%u,%u,%u,%u gl=0x%x\n", fence && color && !glStatus ? "PASS" : "FAIL",
               i + 1, getpid(), after.ownerPID, after.sequence, after.ticket, after.fence, after.pixels,
               rgba[0], rgba[1], rgba[2], rgba[3], glStatus);
        ok = ok && fence && color && !glStatus;
    }
done:
    if (context) {
        current(0);
        destroyContext(context);
    }
    if (format)
        destroyFormat(format);
    dlclose(openGL);
    return ok;
}

static int checkFormat(const Registry *api, io_service_t gpu, const char *bundle)
{
    void *gld = dlopen(bundle, kDlopenNow);
    if (!gld) {
        printf("XenosCompositorContext GLD load failed %s\n", dlerror());
        return 0;
    }
    void (*initialize)(const void *, const void *, UInt32, UInt32, UInt32) =
        dlsym(gld, "gldInitializeLibrary");
    int (*choose)(void **, const int *) = dlsym(gld, "gldChoosePixelFormat");
    int (*query)(void *, UInt32) = dlsym(gld, "gldGetRendererInfo");
    int (*destroy)(void *) = dlsym(gld, "gldDestroyPixelFormat");
    if (!initialize || !choose || !query || !destroy) {
        dlclose(gld);
        return 0;
    }
    initialize(0, 0, 1, 0, 0);
    UInt32 info[14] = {0};
    const int rendererStatus = query(info, 1);
    const int compositor[] = {80, 20, 8, 21, 8, 22, 8, 12, 0, 13, 0, 0};
    XenosGPUCompletion before = {.sequence = 0}, after = {.sequence = 0}, repeated = {.sequence = 0};
    snapshot(api, gpu, &before);
    void *format = 0;
    int result = choose(&format, compositor);
    const int matched = !result && format && ((const UInt32 *)format)[1] == kXenosCompositorGLDID;
    const int fresh = snapshot(api, gpu, &after) && after.sequence != before.sequence &&
        after.ownerPID == (UInt32)getpid() && after.ticket && after.ticket == after.fence &&
        after.state == kXenosQuadPassed && after.pixels == kXenosQuadReadbackPixels && !after.mismatches;
    if (format)
        destroy(format);
    format = 0;
    const int repeat = choose(&format, compositor);
    const int once = snapshot(api, gpu, &repeated) && repeated.sequence == after.sequence;
    if (format)
        destroy(format);
    const int ok = !rendererStatus && info[1] == kXenosCompositorGLDID &&
        !(info[2] & 0x1000U) && matched && fresh && !repeat && once;
    printf("XenosCompositorContext %s format=%d pid=%d owner=%u ticket=%u fence=%u pixels=%u "
           "vram_report=%u one_shot=%d\n", ok ? "PASS" : "FAIL", matched, getpid(), after.ownerPID,
           after.ticket, after.fence, after.pixels, info[12], once);
    dlclose(gld);
    return ok;
}

int main(int argc, char **argv)
{
    if (argc != 1 && (argc != 3 || strcmp(argv[1], "--format")))
        return 2;
    Registry api = {.io = 0};
    if (!openRegistry(&api))
        return 1;
    io_service_t gpu = api.find(0, api.matching("XenosController"));
    int result = gpu && (argc == 1 ? checkContext(&api, gpu) : checkFormat(&api, gpu, argv[2]));
    if (gpu)
        api.releaseObject(gpu);
    dlclose(api.cf);
    dlclose(api.io);
    return !result;
}
