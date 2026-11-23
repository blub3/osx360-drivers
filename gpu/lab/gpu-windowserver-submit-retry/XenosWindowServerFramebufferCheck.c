#include "Offscreen.h"
#include "WindowGL.h"
#include "XenosWindowServerFramebufferProbe.h"

extern int getpid(void);
extern unsigned long strtoul(const char *, char **, int);

typedef const void *CFRef;
typedef void *WindowRef;
typedef struct { short top, left, bottom, right; } QDRect;
typedef struct { unsigned short red, green, blue; } RGBColor;

enum { kWindowClass = 13, kWindowNoShadow = 1U << 21, kWindowContentRegion = 33,
       kCFNumberSInt32Type = 3, kCFStringASCII = 0x0600,
       kBaseline = 0, kRequired = 1, kControl = 2, kAssociation = 3, kStartup = 4 };

typedef struct {
    int (*qeEnabled)(CGDirectDisplayID);
    io_service_t (*displayService)(CGDirectDisplayID);
    int (*queryRenderers)(UInt32, CGLRendererInfoObj *, int *);
    int (*describeRenderer)(CGLRendererInfoObj, int, int, int *);
    int (*destroyRendererInfo)(CGLRendererInfoObj);
    int (*conforms)(io_object_t, const char *);
    int (*findAccelerator)(io_service_t, io_service_t *, UInt32 *);
    void *(*serviceMatching)(const char *);
    io_service_t (*matchingService)(UInt32, void *);
    int (*getParent)(io_object_t, const char *, io_object_t *);
    CFRef (*createProperty)(io_object_t, CFRef, CFRef, UInt32);
    int (*releaseObject)(io_object_t);
    CFRef (*createString)(CFRef, const char *, UInt32);
    unsigned char (*stringValue)(CFRef, char *, long, UInt32);
    unsigned char (*numberValue)(CFRef, int, void *);
    const unsigned char *(*dataPointer)(CFRef);
    long (*dataLength)(CFRef);
    unsigned long (*type)(CFRef);
    unsigned long (*dataType)(void);
    unsigned long (*stringType)(void);
    unsigned long (*numberType)(void);
    void (*releaseCF)(CFRef);
    int (*createWindow)(UInt32, UInt32, const QDRect *, WindowRef *);
    void (*disposeWindow)(WindowRef);
    void (*showWindow)(WindowRef);
    void (*moveWindow)(WindowRef, short, short, unsigned char);
    int (*windowBounds)(WindowRef, unsigned, QDRect *);
    void *(*windowPort)(WindowRef);
    void (*runEvents)(double);
    void (*getPort)(void **);
    void (*setPort)(void *);
    void (*getPortBounds)(void *, QDRect *);
    void (*paintRect)(const QDRect *);
    void (*foreColor)(const RGBColor *);
    void (*flushPort)(void *, void *);
} API;

static int loadAPI(API *api)
{
    void *cg = dlopen("/System/Library/Frameworks/ApplicationServices.framework/Versions/A/"
                      "Frameworks/CoreGraphics.framework/Versions/A/CoreGraphics", kDlopenNow);
    void *cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation",
                      kDlopenNow);
    void *io = dlopen("/System/Library/Frameworks/IOKit.framework/Versions/A/IOKit", kDlopenNow);
    void *cgl = dlopen("/System/Library/Frameworks/OpenGL.framework/Versions/A/OpenGL", kDlopenNow);
    void *hit = dlopen("/System/Library/Frameworks/Carbon.framework/Versions/A/Frameworks/"
                       "HIToolbox.framework/Versions/A/HIToolbox", kDlopenNow);
    void *qd = dlopen("/System/Library/Frameworks/ApplicationServices.framework/Versions/A/"
                      "Frameworks/QD.framework/Versions/A/QD", kDlopenNow);
    if (!cg || !cf || !io || !cgl || !hit || !qd) {
        printf("XenosWindowServer FAIL loading Tiger frameworks: %s\n", dlerror());
        return 0;
    }
#define LOAD(library, field, symbol) do { \
    api->field = dlsym(library, #symbol); \
    if (!api->field) { printf("XenosWindowServer FAIL missing %s\n", #symbol); return 0; } \
} while (0)
    LOAD(cg, qeEnabled, CGDisplayUsesOpenGLAcceleration);
    LOAD(cg, displayService, CGDisplayIOServicePort);
    LOAD(cgl, queryRenderers, CGLQueryRendererInfo);
    LOAD(cgl, describeRenderer, CGLDescribeRenderer);
    LOAD(cgl, destroyRendererInfo, CGLDestroyRendererInfo);
    LOAD(io, conforms, IOObjectConformsTo);
    LOAD(io, findAccelerator, IOAccelFindAccelerator);
    LOAD(io, serviceMatching, IOServiceMatching);
    LOAD(io, matchingService, IOServiceGetMatchingService);
    LOAD(io, getParent, IORegistryEntryGetParentEntry);
    LOAD(io, createProperty, IORegistryEntryCreateCFProperty);
    LOAD(io, releaseObject, IOObjectRelease);
    LOAD(cf, createString, CFStringCreateWithCString);
    LOAD(cf, stringValue, CFStringGetCString);
    LOAD(cf, numberValue, CFNumberGetValue);
    LOAD(cf, dataPointer, CFDataGetBytePtr);
    LOAD(cf, dataLength, CFDataGetLength);
    LOAD(cf, type, CFGetTypeID);
    LOAD(cf, dataType, CFDataGetTypeID);
    LOAD(cf, stringType, CFStringGetTypeID);
    LOAD(cf, numberType, CFNumberGetTypeID);
    LOAD(cf, releaseCF, CFRelease);
    LOAD(hit, createWindow, CreateNewWindow);
    LOAD(hit, disposeWindow, DisposeWindow);
    LOAD(hit, showWindow, ShowWindow);
    LOAD(hit, moveWindow, MoveWindow);
    LOAD(hit, windowBounds, GetWindowBounds);
    LOAD(hit, windowPort, GetWindowPort);
    LOAD(hit, runEvents, RunCurrentEventLoop);
    LOAD(qd, getPort, GetPort);
    LOAD(qd, setPort, SetPort);
    LOAD(qd, getPortBounds, GetPortBounds);
    LOAD(qd, paintRect, PaintRect);
    LOAD(qd, foreColor, RGBForeColor);
    LOAD(qd, flushPort, QDFlushPortBuffer);
#undef LOAD
    return 1;
}

static CFRef property(const API *api, io_object_t service, const char *name)
{
    CFRef key = api->createString(0, name, kCFStringASCII);
    if (!key)
        return 0;
    CFRef value = api->createProperty(service, key, 0, 0);
    api->releaseCF(key);
    return value;
}

static int stringProperty(const API *api, io_object_t service, const char *name,
                          char *out, long size)
{
    CFRef value = property(api, service, name);
    if (!value)
        return 0;
    int available = api->type(value) == api->stringType() &&
                    api->stringValue(value, out, size, kCFStringASCII);
    api->releaseCF(value);
    return available;
}

static UInt32 readRegistryWord(const API *api, io_service_t service, const char *name)
{
    UInt32 word = 0;
    CFRef value = property(api, service, name);
    if (value) {
        if (api->type(value) == api->numberType())
            api->numberValue(value, kCFNumberSInt32Type, &word);
        api->releaseCF(value);
    }
    return word;
}

static UInt32 legacyFence(const API *api, io_service_t controller)
{
    UInt32 fence = 0;
    CFRef value = property(api, controller, "XenosUserQuadFence");
    if (value) {
        if (api->type(value) == api->numberType())
            api->numberValue(value, kCFNumberSInt32Type, &fence);
        api->releaseCF(value);
    }
    return fence;
}

static int completion(const API *api, io_service_t controller, XenosGPUCompletion *out)
{
    CFRef value = property(api, controller, XENOS_GPU_COMPLETION_KEY);
    if (!value)
        return 0;
    int available = api->type(value) == api->dataType() && api->dataLength(value) == sizeof(*out);
    if (available)
        memcpy(out, api->dataPointer(value), sizeof(*out));
    api->releaseCF(value);
    return available;
}

static int startupTrace(const API *api, io_service_t controller, XenosWindowServerStartupTrace *out)
{
    CFRef value = property(api, controller, XENOS_WS_STARTUP_TRACE_KEY);
    if (!value)
        return 0;
    int available = api->type(value) == api->dataType() && api->dataLength(value) == sizeof(*out);
    if (available)
        memcpy(out, api->dataPointer(value), sizeof(*out));
    api->releaseCF(value);
    return available && out->count <= kXenosWSStartupClears;
}

static int verified(const XenosGPUCompletion *entry, UInt32 ownerPID)
{
    return entry->sequence && entry->ownerPID == ownerPID && entry->ticket &&
           entry->fence == entry->ticket && entry->state == kXenosQuadPassed &&
           entry->pixels && !entry->mismatches;
}

static void printCompletion(const char *label, const XenosGPUCompletion *entry)
{
    printf("XenosWindowServer %s seq=%u owner_pid=%u ticket=%u fence=%u state=%u pixels=%u mismatches=%u\n",
           label, entry->sequence, entry->ownerPID, entry->ticket, entry->fence,
           entry->state, entry->pixels, entry->mismatches);
}

static int checkStartup(const API *api, io_service_t controller, UInt32 windowServer)
{
    XenosWindowServerStartupTrace trace;
    if (!startupTrace(api, controller, &trace)) {
        printf("XenosWindowServer FAIL no startup GPU trace for pid=%u\n", windowServer);
        return 0;
    }
    const UInt32 expected[] = {0x3d4ed5c5U, 0x69e21dc5U};
    UInt32 lastSequence = 0;
    int ok = trace.count == kXenosWSStartupClears;
    for (unsigned i = 0; i < trace.count; ++i) {
        const XenosWindowServerStartupFence *entry = &trace.fences[i];
        const XenosGPUCompletion *fence = &entry->completion;
        printCompletion("WindowServer-framebuffer-open", fence);
        printf("XenosWindowServer startup checksum=%08x expected=%08x\n", entry->hash, expected[i]);
        ok = ok && verified(fence, windowServer) && fence->sequence > lastSequence &&
             fence->pixels == kXenosSurfaceSide*kXenosSurfaceSide && entry->hash == expected[i];
        lastSequence = fence->sequence;
    }
    printf("XenosWindowServer %s startup GPU fence ownership pid=%u count=%u\n",
           ok ? "PASS" : "FAIL", windowServer, trace.count);
    return ok;
}

static int isStartupFence(const API *api, io_service_t controller, UInt32 sequence)
{
    XenosWindowServerStartupTrace trace;
    if (!startupTrace(api, controller, &trace))
        return 0;
    for (unsigned i = 0; i < trace.count; ++i) {
        if (trace.fences[i].completion.sequence == sequence)
            return 1;
    }
    return 0;
}

static io_service_t findFramebuffer(const API *api, CGDirectDisplayID display)
{
    const io_service_t borrowed = api->displayService(display);
    for (io_service_t node = borrowed; node;) {
        if (api->conforms(node, "XenonFramebufferTest"))
            return node;
        io_service_t parent = 0;
        api->getParent(node, "IOService", &parent);
        if (node != borrowed)
            api->releaseObject(node);
        node = parent;
    }
    return 0;
}

static int enumerate(const API *api, UInt32 mask, int *xenosAccelerated)
{
    CGLRendererInfoObj info = 0;
    int count = 0;
    int error = api->queryRenderers(mask, &info, &count);
    printf("XenosWindowServer native_cgl_error=%d renderers=%d\n", error, count);
    if (error || !info || count < 1)
        return 0;
    int readable = 1;
    for (int i = 0; i < count; ++i) {
        int id = 0, accelerated = -1;
        if (api->describeRenderer(info, i, kCGLRPRendererID, &id) ||
            api->describeRenderer(info, i, kCGLRPAccelerated, &accelerated)) {
            readable = 0;
            break;
        }
        printf("XenosWindowServer native_cgl_renderer=%08x accelerated=%d\n", (UInt32)id, accelerated);
        if ((UInt32)id == kXenosCGLRendererID && accelerated)
            *xenosAccelerated = 1;
    }
    return api->destroyRendererInfo(info) == 0 && readable;
}

static void paint(const API *api, WindowRef window, RGBColor color)
{
    void *saved = 0, *port = api->windowPort(window);
    QDRect bounds;
    api->getPort(&saved);
    api->getPortBounds(port, &bounds);
    api->setPort(port);
    api->foreColor(&color);
    api->paintRect(&bounds);
    api->flushPort(port, 0);
    api->setPort(saved);
}

static int moveWindows(const API *api, io_service_t controller, UInt32 owner, unsigned *witnesses)
{
    const QDRect backFrame = {160, 182, 256, 310};
    const QDRect frontFrame = {183, 216, 255, 312};
    WindowRef back = 0, front = 0;
    int success = 0;
    XenosGPUCompletion last = {.sequence = 0};
    completion(api, controller, &last);
    if (api->createWindow(kWindowClass, kWindowNoShadow, &backFrame, &back) || !back ||
        api->createWindow(kWindowClass, kWindowNoShadow, &frontFrame, &front) || !front)
        goto done;
    api->showWindow(back);
    api->showWindow(front);
    api->runEvents(0.10);
    const RGBColor base = {0x3535, 0x7979, 0xb4b4};
    paint(api, back, base);
    printf("XenosWindowServer workload=overlapping-window-repaints-and-moves count=8\n");
    for (int i = 0; i < 8; ++i) {
        const short x = 216 + (i & 1) * 28, y = 183 + (i & 1) * 12;
        const RGBColor color = {0xb500 + 431 * i, 0x3800 + 872 * i, 0x7100 - 516 * i};
        api->moveWindow(front, x, y, 1);
        paint(api, front, color);
        api->runEvents(0.13);
        QDRect moved;
        if (api->windowBounds(front, kWindowContentRegion, &moved) ||
            moved.left != x || moved.top != y)
            goto done;
        for (int poll = 0; poll < 3; ++poll) {
            XenosGPUCompletion current;
            if (completion(api, controller, &current) && current.sequence != last.sequence) {
                if (verified(&current, owner) && !isStartupFence(api, controller, current.sequence)) {
                    ++*witnesses;
                    printCompletion("WindowServer-fence", &current);
                }
                last = current;
            }
            api->runEvents(0.03);
        }
    }
    success = 1;
done:
    if (front)
        api->disposeWindow(front);
    if (back)
        api->disposeWindow(back);
    return success;
}

static int runControl(const API *api, io_service_t controller)
{
    XenosBackend backend = {.connection = 0};
    if (!XenosOpenBackend(&backend))
        return 0;
    XenosQuadGeometryUpload upload;
    upload.geometry = xenosQuadMakeGeometry(5, 7, 21, 23, 0, 0, 16, 16);
    for (unsigned i = 0; i < kXenosQuadTexturePixels; ++i) {
        upload.rgb[3*i] = (unsigned char)(57 + i);
        upload.rgb[3*i+1] = (unsigned char)(141 - i);
        upload.rgb[3*i+2] = (unsigned char)(32 + 7*i);
    }
    UInt32 pixels[kXenosQuadReadbackPixels];
    XenosFence fence;
    int ok = XenosRenderQuad(&backend, &upload, 0, &fence, pixels);
    XenosCloseBackend(&backend);
    if (!ok)
        return 0;
    UInt32 mismatch = 0;
    for (unsigned y = 0; y < kXenosQuadTargetSide; ++y) {
        for (unsigned x = 0; x < kXenosQuadTargetSide; ++x) {
            UInt32 expected = kBackground;
            if (x >= 5 && x < 21 && y >= 7 && y < 23) {
                unsigned i = (y - 7)*16 + x - 5;
                expected = 0xff000000U | (UInt32)upload.rgb[3*i] << 16 |
                           (UInt32)upload.rgb[3*i+1] << 8 | upload.rgb[3*i+2];
            }
            mismatch += pixels[y * kXenosQuadTargetSide + x] != expected;
        }
    }
    XenosGPUCompletion entry;
    const int haveTrace = completion(api, controller, &entry);
    if (haveTrace)
        printCompletion("client-control", &entry);
    if (mismatch || legacyFence(api, controller) != fence.fence ||
        (haveTrace && (!verified(&entry, (UInt32)getpid()) || fence.ticket != entry.ticket))) {
        printf("XenosWindowServer FAIL GPU client control pixels_mismatched=%u\n", mismatch);
        return 0;
    }
    printf("XenosWindowServer PASS GPU client control pid=%d ticket=%u fence=%u pixels=%u owner_trace=%s\n",
           getpid(), fence.ticket, fence.fence, fence.pixels,
           haveTrace ? "verified" : "unavailable-on-this-kext");
    return 1;
}

int main(int argc, char **argv)
{
    char *end = 0;
    if (argc != 3)
        return 2;
    const UInt32 windowServer = strtoul(argv[2], &end, 10);
    if (!windowServer || !end || *end)
        return 2;
    const int mode = !strcmp(argv[1], "--baseline") ? kBaseline :
                     !strcmp(argv[1], "--require-native") ? kRequired :
                     !strcmp(argv[1], "--control") ? kControl :
                     !strcmp(argv[1], "--association") ? kAssociation :
                     !strcmp(argv[1], "--startup") ? kStartup : -1;
    if (mode == -1)
        return 2;
    API api;
    XenosDisplay display;
    if (!loadAPI(&api) || !XenosFindDisplay(&display))
        return 1;
    io_service_t controller = api.matchingService(0, api.serviceMatching("XenosController"));
    io_service_t framebuffer = findFramebuffer(&api, display.display);
    if (!controller || !framebuffer)
        return 1;
    if (mode == kControl) {
        const int ok = runControl(&api, controller);
        api.releaseObject(controller);
        if (framebuffer != api.displayService(display.display))
            api.releaseObject(framebuffer);
        return !ok;
    }
    const int qeBefore = api.qeEnabled(display.display) != 0;
    io_service_t accelerator = 0;
    UInt32 index = 0;
    const int result = api.findAccelerator(framebuffer, &accelerator, &index);
    const int native = !result && accelerator && api.conforms(accelerator, "XenosDisplayAccelerator");
    char bundle[96];
    const int bundleFound = stringProperty(&api, framebuffer, "IOGLBundleName", bundle, sizeof(bundle));
    char association[512];
    const int associationFound = stringProperty(&api, framebuffer, "IOAccelTypes", association, sizeof(association));
    printf("XenosWindowServer display=0x%08x size=%lux%lu pid=%u QuartzExtreme=%d\n",
           display.display, display.width, display.height, windowServer, qeBefore);
    printf("XenosWindowServer IOAccelFindAccelerator=0x%08x association=%s xenos_service=%d index=%u IOGLBundleName=%s\n",
           (UInt32)result, associationFound ? association : "(absent)",
           native, index, bundleFound ? bundle : "(absent)");
    if (mode == kStartup) {
        const UInt32 openPID = readRegistryWord(&api, framebuffer, XENOS_WS_FRAMEBUFFER_PID);
        const UInt32 queuedPID = readRegistryWord(&api, controller, XENOS_WS_FRAMEBUFFER_QUEUED_PID);
        printf("XenosWindowServer framebuffer_open_pid=%u queued_pid=%u expected_pid=%u\n",
               openPID, queuedPID, windowServer);
        const int fences = checkStartup(&api, controller, windowServer);
        const int ok = native && index == 0 && openPID == windowServer && queuedPID == windowServer && fences;
        if (accelerator)
            api.releaseObject(accelerator);
        api.releaseObject(controller);
        if (framebuffer != api.displayService(display.display))
            api.releaseObject(framebuffer);
        return !ok;
    }
    const UInt32 before = legacyFence(&api, controller);
    unsigned witnesses = 0;
    int xenosAccelerated = 0;
    if (!enumerate(&api, display.mask, &xenosAccelerated) || !moveWindows(&api, controller, windowServer, &witnesses))
        return 1;
    const UInt32 after = legacyFence(&api, controller);
    const int qeAfter = api.qeEnabled(display.display) != 0;
    printf("XenosWindowServer QuartzExtreme_before=%d after=%d WindowServer_fences=%u driver_fence_before=%u after=%u\n",
           qeBefore, qeAfter, witnesses, before, after);
    if (accelerator)
        api.releaseObject(accelerator);
    api.releaseObject(controller);
    if (framebuffer != api.displayService(display.display))
        api.releaseObject(framebuffer);
    if (mode == kAssociation) {
        const int ok = native && associationFound && !qeBefore && !qeAfter && !witnesses;
        printf("XenosWindowServer %s IOAccelerator association with software compositor\n",
               ok ? "PASS" : "FAIL");
        return !ok;
    }
    if (qeBefore && qeAfter && native && xenosAccelerated && bundleFound &&
        !strcmp(bundle, "GLDriverXenosNative") && witnesses >= 2) {
        printf("XenosWindowServer PASS active Quartz Extreme with Xenos WindowServer GPU fences\n");
        return 0;
    }
    printf("XenosWindowServer BLOCKED Quartz Extreme or repaint GPU fence criteria incomplete\n");
    return mode == kBaseline && !qeBefore && !qeAfter && !witnesses && before == after ? 0 : 1;
}
