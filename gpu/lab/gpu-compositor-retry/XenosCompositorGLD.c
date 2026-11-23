#include "Offscreen.h"
#include "XenosCompositorProtocol.h"

extern int _NSGetExecutablePath(char *, UInt32 *);
extern char *realpath(const char *, char *);
extern int getpid(void);
extern int access(const char *, int);
typedef struct {
    long seconds;
    int microseconds;
} TigerTime;
extern int gettimeofday(TigerTime *, void *);
extern void syslog(int, const char *, ...);
extern int OSSpinLockTry(volatile int *);
extern void OSSpinLockUnlock(volatile int *);

enum {
    kWindowRequest = 80, kOffscreenRequest = 53, kColorSize = 8, kAlphaSize = 11,
    kReadAccess = 4, kNotice = 5, kMaxProbeFrames = 8,
    kDrawableOffscreen = 0x35, kDrawableSurface = 0x50, kStockRenderer = 0x0200,
    kDrawableUnchanged = 0, kDrawableAttached = 2, kDrawableResized = 3
};

typedef struct TigerFormat {
    struct TigerFormat *next;
    UInt32 renderer, flags, buffering, colors;
    unsigned char remaining[32];
} TigerFormat;
typedef char TigerFormatSize[sizeof(TigerFormat) == 52 ? 1 : -1];

/* Non-intercepted GLD exports use PPC tail calls and preserve the entire ABI. */
#define GLD_DELEGATE(name) extern void *XenosStock_##name;
#include "GLDDelegate.def"
#undef GLD_DELEGATE

static void (*initStock)(const void *, const void *, UInt32, UInt32, UInt32);
static int (*versionStock)(int *, int *, int *, int *);
static int (*rendererStock)(void *, UInt32);
static int (*chooseStock)(TigerFormat **, const int *);
static int (*createStock)(void **, const TigerFormat *, void *, void *, void *, void *, void *);
static int (*attachStock)(void *, unsigned, const void *, unsigned);
static int loaded, server, privateTest, formatTest;
static UInt32 displayMask;
static volatile int contextLock;
static unsigned probeFrames, formatSubmitted;
static unsigned long long lastProbeTime;

static void probeRepaint(unsigned type);
static void probeTransition(void);

static int isWindowServer(void)
{
    static const char path[] = "/System/Library/Frameworks/ApplicationServices.framework/"
        "Frameworks/CoreGraphics.framework/Resources/WindowServer";
    char executable[1024], actual[1024], expected[1024];
    UInt32 size = sizeof(executable);
    return !_NSGetExecutablePath(executable, &size) &&
           realpath(executable, actual) && realpath(path, expected) && !strcmp(actual, expected);
}

void gldInitializeLibrary(const void *services, const void *indices, UInt32 mask,
                          UInt32 flags, UInt32 options)
{
    if (loaded)
        return;
    const char *armed = getenv("XENOS_GLD_COMPOSITOR_RETRY_TEST");
    server = isWindowServer();
    privateTest = armed && !strcmp(armed, "1");
    formatTest = armed && !strcmp(armed, "format");
    if (!server && !privateTest && !formatTest)
        return;
    void *stock = dlopen("/System/Library/Frameworks/OpenGL.framework/Resources/"
                        "GLDriver.bundle/GLDriver", kDlopenNow);
    if (!stock) {
        if (privateTest) printf("XenosCompositorRetryGLD stock dlopen failed %s\n", dlerror());
        return;
    }
#define LOAD(field, name) do { field = dlsym(stock, #name); if (!field) { if (privateTest) printf("XenosCompositorRetryGLD missing %s\n", #name); goto fail; } } while (0)
    LOAD(initStock, gldInitializeLibrary);
    LOAD(versionStock, gldGetVersion);
    LOAD(rendererStock, gldGetRendererInfo);
    LOAD(chooseStock, gldChoosePixelFormat);
    LOAD(createStock, gldCreateContext);
    LOAD(attachStock, gldAttachDrawable);
#define GLD_DELEGATE(name) LOAD(XenosStock_##name, name);
#include "GLDDelegate.def"
#undef GLD_DELEGATE
#undef LOAD
    displayMask = mask;
    initStock(services, indices, mask, flags, options);
    loaded = 1;
    if (privateTest || formatTest) printf("XenosCompositorRetryGLD private mask=%08x\n", mask);
    if (server)
        syslog(kNotice, "XenosCompositorRetryGLD initialized pid=%d mask=%08x", getpid(), displayMask);
    return;
fail:
    dlclose(stock);
}

int gldGetVersion(int *major, int *minor, int *revision, int *renderer)
{
    if (!loaded || !versionStock(major, minor, revision, renderer) || *renderer != kStockRenderer)
        return 0;
    *renderer = kXenosCompositorGLDID;
    return 1;
}

typedef struct {
    UInt32 next, renderer, flags, characteristics[9];
    UInt32 videoMemory;
    UInt32 textureMemory;
} TigerRendererInfo;

int gldGetRendererInfo(TigerRendererInfo *info, UInt32 mask)
{
    if (!loaded || !mask || !(mask & displayMask))
        return kCGLBadMatch;
    int result = rendererStock(info, mask);
    if (!result) {
        info->renderer = kXenosCompositorGLDID;
        info->flags &= ~0x1000U; /* The stock software rasterizer owns desktop pixels. */
    }
    return result;
}

static int matches(const int *attributes, const int *signature, unsigned size)
{
    for (unsigned i = 0; i < size; ++i)
        if (attributes[i] != signature[i])
            return 0;
    return 1;
}

int gldChoosePixelFormat(TigerFormat **out, const int *attributes)
{
    if (!out)
        return kCGLBadMatch;
    *out = 0;
    if (!loaded || !attributes)
        return 0;
    if (privateTest)
        printf("XenosCompositorRetryGLD choose attr0=%x attr1=%x attr2=%x\n",
               attributes[0], attributes[0] ? attributes[1] : 0,
               attributes[0] && attributes[1] ? attributes[2] : 0);
    const int compositor[] = {kWindowRequest, 20, 8, 21, 8, 22, 8, 12, 0, 13, 0, 0};
    const int privateSignature[] = {kCGLPFARendererID, kXenosCompositorRendererID, kOffscreenRequest, 0};
    const int privateNormalized[] = {kOffscreenRequest, 4, 0};
    const int privateGLD[] = {kCGLPFARendererID, kXenosCompositorGLDID, kOffscreenRequest, 0};
    const int compositorRequest = (server || formatTest) &&
        matches(attributes, compositor, sizeof(compositor) / sizeof(compositor[0]));
    if (!compositorRequest &&
        !(privateTest && (matches(attributes, privateSignature, sizeof(privateSignature) / sizeof(privateSignature[0])) ||
                          matches(attributes, privateGLD, sizeof(privateGLD) / sizeof(privateGLD[0])) ||
                          matches(attributes, privateNormalized, sizeof(privateNormalized) / sizeof(privateNormalized[0])))))
        return 0;
    const int stock[] = {kOffscreenRequest, 4, kColorSize, 32, kAlphaSize, 8, 0};
    int result = chooseStock(out, stock);
    for (TigerFormat *format = *out; format; format = format->next)
        format->renderer = kXenosCompositorGLDID;
    if (privateTest) printf("XenosCompositorRetryGLD stock choose status=%d format=%p\n", result, (void *)*out);
    if (compositorRequest) {
        if (server)
            syslog(kNotice, "XenosCompositorRetryGLD compositor format status=%d available=%d", result, *out != 0);
        if (!result && *out)
            probeTransition();
    }
    return result;
}

int gldCreateContext(void **result, const TigerFormat *format, void *shared,
                     void *unused, void *configuration, void *functions, void *userdata)
{
    int status = createStock(result, format, shared, unused, configuration, functions, userdata);
    if (!status && result && *result) {
        if (server)
            syslog(kNotice, "XenosCompositorRetryGLD context created pid=%d", getpid());
        else if (privateTest)
            printf("XenosCompositorRetryGLD context created pid=%d\n", getpid());
    }
    return status;
}

int gldAttachDrawable(void *context, unsigned type, const void *drawable, unsigned screen)
{
    int result = attachStock(context, type, drawable, screen);
    /* GLEngine treats an unchanged, attached or resized drawable as success. */
    if (result == kDrawableUnchanged || result == kDrawableAttached || result == kDrawableResized)
        probeRepaint(type);
    else if (server && type)
        syslog(kNotice, "XenosCompositorRetryGLD drawable attachment type=0x%x status=%d", type, result);
    return result;
}

static void submitDiagnostic(const char *source, unsigned frame)
{
    XenosQuadGeometryUpload upload;
    upload.geometry = xenosQuadMakeGeometry(5, 7, 21, 23, 0, 0, 16, 16);
    for (unsigned i = 0; i < kXenosQuadTexturePixels; ++i) {
        upload.rgb[3*i] = (unsigned char)(29 + i + 17*frame);
        upload.rgb[3*i + 1] = (unsigned char)(211 - i);
        upload.rgb[3*i + 2] = (unsigned char)(74 + 9*i);
    }
    XenosBackend backend = {.connection = 0};
    if (!XenosOpenBackend(&backend)) {
        if (server)
            syslog(kNotice, "XenosCompositorRetryGLD FAIL opening GPU connection");
        if (privateTest) printf("XenosCompositorRetryGLD FAIL opening GPU connection\n");
        return;
    }
    XenosFence fence;
    UInt32 pixels[kXenosQuadReadbackPixels];
    const int success = XenosRenderQuad(&backend, &upload, 0, &fence, pixels);
    XenosCloseBackend(&backend);
    if (!success && server)
        syslog(kNotice, "XenosCompositorRetryGLD FAIL GPU readback pid=%d frame=%u", getpid(), frame);
    if (success) {
        if (server)
            syslog(kNotice, "XenosCompositorRetryGLD source=%s pid=%d frame=%u ticket=%u fence=%u pixels=%u hash=%08x",
                   source, getpid(), frame, fence.ticket, fence.fence, fence.pixels, fence.hash);
        else
            printf("XenosCompositorRetryGLD source=%s pid=%d frame=%u ticket=%u fence=%u pixels=%u hash=%08x\n",
                   source, getpid(), frame, fence.ticket, fence.fence, fence.pixels, fence.hash);
    }
}


static void probeTransition(void)
{
    static const char arm[] = "/Volumes/OSX360BOOT/XenosCompositorRetryTest/transition-probe.arm";
    if ((!formatTest && (!server || access(arm, kReadAccess))) || !OSSpinLockTry(&contextLock))
        return;

    /* A transition can initialize CGLS on a framebuffer without a window
       surface engine. This fence records the format callback, before attachment. */
    unsigned submit = !formatSubmitted;
    formatSubmitted = 1;
    OSSpinLockUnlock(&contextLock);
    if (submit)
        submitDiagnostic("transition-format", 1);
}

static void probeRepaint(unsigned type)
{
    static const char arm[] = "/Volumes/OSX360BOOT/XenosCompositorRetryTest/repaint-probe.arm";
    if (type != (server ? kDrawableSurface : kDrawableOffscreen) ||
        (server && access(arm, kReadAccess)))
        return;
    unsigned long long now = 0;
    if (server) {
        TigerTime time;
        if (gettimeofday(&time, 0))
            return;
        now = (unsigned long long)time.seconds * 1000000 + time.microseconds;
    }
    if (!OSSpinLockTry(&contextLock))
        return;
    if (probeFrames >= kMaxProbeFrames ||
        (server && now >= lastProbeTime && now - lastProbeTime < 180000)) {
        OSSpinLockUnlock(&contextLock);
        return;
    }
    lastProbeTime = now;
    const unsigned frame = ++probeFrames;
    OSSpinLockUnlock(&contextLock);

    submitDiagnostic(server ? "repaint-drawable" : "offscreen-drawable", frame);
}
