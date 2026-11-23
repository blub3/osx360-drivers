#include "XenosWindowCopy.h"
#include "WindowGL.h"

extern int open(const char *, int, ...);
extern int close(int);
extern long write(int, const void *, unsigned long);
extern int unlink(const char *);
extern unsigned long strtoul(const char *, char **, int);

typedef void *WindowRef;
typedef struct { short top, left, bottom, right; } QDRect;
typedef struct { unsigned short red, green, blue; } RGBColor;

enum { kPlainWindowClass = 13, kWindowNoShadow = 1U << 21, kWindowContent = 33,
       kWriteOnly = 1, kCreate = 0x0200, kExclusive = 0x0800 };

typedef struct {
    void *io;
    void *(*serviceMatching)(const char *);
    UInt32 (*matchingService)(UInt32, void *);
    int (*releaseObject)(UInt32);
    CFRef (*property)(UInt32, CFRef, CFRef, UInt32);
    CFRef (*str)(CFRef, const char *, UInt32);
    unsigned long (*type)(CFRef);
    unsigned long (*dataType)(void);
    long (*dataLength)(CFRef);
    const unsigned char *(*dataBytes)(CFRef);
    void (*release)(CFRef);
    void *(*displayAddress)(CGDirectDisplayID);
    unsigned long (*stride)(CGDirectDisplayID);
    unsigned long (*bits)(CGDirectDisplayID);
    int (*qe)(CGDirectDisplayID);
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
    void *hit = dlopen("/System/Library/Frameworks/Carbon.framework/Versions/A/Frameworks/"
                       "HIToolbox.framework/Versions/A/HIToolbox", kDlopenNow);
    void *qd = dlopen("/System/Library/Frameworks/ApplicationServices.framework/Versions/A/"
                      "Frameworks/QD.framework/Versions/A/QD", kDlopenNow);
    api->io = dlopen("/System/Library/Frameworks/IOKit.framework/Versions/A/IOKit", kDlopenNow);
    if (!cg || !cf || !api->io || !hit || !qd)
        return 0;
#define LOAD(lib, field, sym) do { \
    api->field = dlsym(lib, #sym); \
    if (!api->field) { printf("Missing Tiger symbol %s\n", #sym); return 0; } \
} while (0)
    LOAD(api->io, serviceMatching, IOServiceMatching);
    LOAD(api->io, matchingService, IOServiceGetMatchingService);
    LOAD(api->io, releaseObject, IOObjectRelease);
    LOAD(api->io, property, IORegistryEntryCreateCFProperty);
    LOAD(cf, str, CFStringCreateWithCString);
    LOAD(cf, type, CFGetTypeID);
    LOAD(cf, dataType, CFDataGetTypeID);
    LOAD(cf, dataLength, CFDataGetLength);
    LOAD(cf, dataBytes, CFDataGetBytePtr);
    LOAD(cf, release, CFRelease);
    LOAD(cg, displayAddress, CGDisplayBaseAddress);
    LOAD(cg, stride, CGDisplayBytesPerRow);
    LOAD(cg, bits, CGDisplayBitsPerPixel);
    LOAD(cg, qe, CGDisplayUsesOpenGLAcceleration);
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

static int completion(const API *api, UInt32 gpu, XenosGPUCompletion *out)
{
    CFRef key = api->str(0, XENOS_GPU_COMPLETION_KEY, 0x0600);
    if (!key)
        return 0;
    CFRef value = api->property(gpu, key, 0, 0);
    api->release(key);
    if (!value)
        return 0;
    const int valid = api->type(value) == api->dataType() &&
                      api->dataLength(value) == sizeof(*out);
    if (valid)
        memcpy(out, api->dataBytes(value), sizeof(*out));
    api->release(value);
    return valid;
}

static unsigned matchRGB(const UInt32 *a, const UInt32 *b)
{
    unsigned count = 0;
    for (int i = 0; i < kWindowCopyPixels; ++i)
        count += ((a[i] ^ b[i]) & 0xffffffU) == 0;
    return count;
}

static int paint(const API *api, WindowRef window)
{
    const RGBColor colors[4] = {{0xf2f2, 0x2626, 0x3636}, {0x1515, 0x9090, 0x4848},
                                {0x2525, 0x4545, 0xc7c7}, {0xcaca, 0xa7a7, 0x1818}};
    void *saved = 0, *port = api->windowPort(window);
    QDRect bounds;
    api->getPortBounds(port, &bounds);
    if (bounds.right - bounds.left != kWindowCopySide ||
        bounds.bottom - bounds.top != kWindowCopySide)
        return 0;
    api->getPort(&saved);
    api->setPort(port);
    for (int i = 0; i < 4; ++i) {
        short x = bounds.left + (i & 1) * (kWindowCopySide/2);
        short y = bounds.top + (i >> 1) * (kWindowCopySide/2);
        QDRect part = {y, x, y + kWindowCopySide/2, x + kWindowCopySide/2};
        api->foreColor(&colors[i]);
        api->paintRect(&part);
    }
    api->flushPort(port, 0);
    api->setPort(saved);
    return 1;
}

static int hasPattern(const UInt32 pixels[kWindowCopyPixels])
{
    const unsigned half = kWindowCopySide/2;
    UInt32 colors[4] = {pixels[0], pixels[half], pixels[half*kWindowCopySide],
                        pixels[half*kWindowCopySide+half]};
    for (unsigned i = 0; i < 4; ++i)
        for (unsigned j = 0; j < i; ++j)
            if (!((colors[i] ^ colors[j]) & 0xffffffU))
                return 0;
    for (unsigned y = 0; y < kWindowCopySide; ++y)
        for (unsigned x = 0; x < kWindowCopySide; ++x)
            if ((pixels[y*kWindowCopySide+x] ^ colors[(y/half)*2+x/half]) & 0xffffffU)
                return 0;
    return 1;
}

static int armCopy(const XenosWindowArm *arm)
{
    int file = open(XENOS_WINDOW_ARM, kWriteOnly | kCreate | kExclusive, 0644);
    if (file < 0)
        return 0;
    const int ok = write(file, arm, sizeof(*arm)) == sizeof(*arm);
    close(file);
    if (!ok)
        unlink(XENOS_WINDOW_ARM);
    return ok;
}

static int windowTest(const API *api, UInt32 expectedOwner)
{
    const int active = expectedOwner != 0;
    const short x = 224, y = 208, destX = 304, destY = 248;
    QDRect frame = {y, x, y + kWindowCopySide, x + kWindowCopySide}, bounds;
    WindowRef window = 0;
    XenosDisplay display;
    if (!XenosFindDisplay(&display) || api->bits(display.display) != 32 ||
        api->stride(display.display) != kFramebufferRowBytes)
        return 0;
    UInt32 fb = api->matchingService(0, api->serviceMatching("XenonFramebufferTest"));
    UInt32 gpu = api->matchingService(0, api->serviceMatching("XenosController"));
    XenosFramebufferMap map = {.connection = 0};
    int ok = 0, armed = 0;
    if (!fb || !gpu || !XenosMapFramebuffer(api->io, fb, &map))
        goto done;
    const XenosWindowPixels mapped = {(volatile UInt32 *)map.address, kFramebufferRowBytes,
                                      display.width, display.height};
    const XenosWindowPixels screen = {api->displayAddress(display.display), kFramebufferRowBytes,
                                      display.width, display.height};
    const int qeBefore = api->qe(display.display) != 0;
    if (!screen.base || api->createWindow(kPlainWindowClass, kWindowNoShadow, &frame, &window) ||
        api->windowBounds(window, kWindowContent, &bounds) || bounds.left != x || bounds.top != y ||
        bounds.right != x + kWindowCopySide || bounds.bottom != y + kWindowCopySide)
        goto done;
    api->showWindow(window);
    api->runEvents(0.15);
    if (!paint(api, window))
        goto done;
    UInt32 source[kWindowCopyPixels], alias[kWindowCopyPixels], target[kWindowCopyPixels];
    int havePattern = 0;
    for (int attempt = 0; attempt < 20; ++attempt) {
        api->runEvents(0.05);
        if (XenosWindowSample(&screen,x,y,source) && XenosWindowSample(&mapped,x,y,alias) &&
            matchRGB(source,alias) == kWindowCopyPixels && hasPattern(source)) {
            havePattern = 1;
            break;
        }
    }
    if (!havePattern || !XenosWindowSample(&screen,destX,destY,target) ||
        matchRGB(source,target) == kWindowCopyPixels) {
        printf("XenosWindowRepaint FAIL verifying distinct source and destination before move pattern=%d\n", havePattern);
        goto done;
    }
    const XenosWindowArm arm = {kGPUWindowCopyMagic, {x, y, destX, destY}, XenosWindowHash(source)};
    XenosGPUCompletion before = {.sequence=0}, after = {.sequence=0};
    const int haveBefore = completion(api, gpu, &before);
    if (!haveBefore || (active && !(armed=armCopy(&arm))))
        goto done;
    api->moveWindow(window,destX,destY,1);
    unsigned matches = 0;
    for (int attempt = 0; attempt < 25 && matches != kWindowCopyPixels; ++attempt) {
        api->runEvents(0.05);
        if (XenosWindowSample(&screen,destX,destY,target))
            matches = matchRGB(source,target);
    }
    const int atTarget = !api->windowBounds(window,kWindowContent,&bounds) &&
        bounds.left == destX && bounds.top == destY;
    const int readCompletion = completion(api,gpu,&after);
    ok = atTarget && matches == kWindowCopyPixels && readCompletion &&
        (active ? after.sequence != before.sequence && after.ownerPID == expectedOwner &&
                  after.ticket && after.fence == after.ticket && after.state == kXenosQuadPassed &&
                  after.pixels == kXenosQuadReadbackPixels && !after.mismatches :
                  after.sequence == before.sequence);
    printf("XenosWindowRepaint %s mode=%s src=%d,%d dst=%d,%d hash=%08x "
           "screen_pixels=%u owner_pid=%u ticket=%u fence=%u gpu_pixels=%u gpu_mismatches=%u "
           "QuartzExtreme_before=%d after=%d\n",
           ok ? "PASS" : "FAIL", active ? "repaint" : "baseline", x, y, destX, destY, arm.hash,
           matches, after.ownerPID, after.ticket, after.fence, after.pixels, after.mismatches,
           qeBefore, api->qe(display.display) != 0);
done:
    if (armed)
        unlink(XENOS_WINDOW_ARM);
    if (window)
        api->disposeWindow(window);
    if (map.connection)
        XenosUnmapFramebuffer(&map);
    if (gpu)
        api->releaseObject(gpu);
    if (fb)
        api->releaseObject(fb);
    return ok;
}

static int privateCopyTest(void)
{
    enum { kSide = 64 };
    UInt32 surface[kSide * kSide], original[kSide * kSide];
    const XenosWindowPixels screen = {surface, kSide * sizeof(UInt32), kSide, kSide};
    XenosWindowArm arm = {kGPUWindowCopyMagic, {5, 7, 37, 42}, 0};
    for (int y = 0; y < kSide; ++y) {
        for (int x = 0; x < kSide; ++x) {
            surface[y*kSide + x] = 0xffd2c1b0U;
            if (x >= 5 && x < 5+kWindowCopySide && y >= 7 && y < 7+kWindowCopySide)
                surface[y*kSide + x] = 0xff000000U |
                    (UInt32)((x*37 + y*11) & 255) << 16 |
                    (UInt32)((x*13 + y*107) & 255) << 8 | (UInt32)((x*83 + y*19) & 255);
        }
    }
    memcpy(original,surface,sizeof(surface));
    UInt32 source[kWindowCopyPixels];
    if (!XenosWindowSample(&screen,5,7,source))
        return 0;
    arm.hash = XenosWindowHash(source);
    const TigerCopy rectangle = {.count=1, .rectangle={5,7,37,42,kWindowCopySide,kWindowCopySide}};
    const struct {
        UInt32 count;
        short bounds[4], rect[4];
    } region = {1, {5,7,kWindowCopySide,kWindowCopySide}, {5,7,kWindowCopySide,kWindowCopySide}};
    const TigerCopyRegion regionCopy = {.deltaX=37, .deltaY=42, .region=&region};
    XenosWindowMove move;
    if (!XenosDecodeWindowMove(kBlitCopy,0,&rectangle,&move) ||
        !XenosWindowMoveMatches(&move,&arm) ||
        !XenosDecodeWindowMove(kBlitCopyRegion,kBlitFramebuffer,&regionCopy,&move) ||
        !XenosWindowMoveMatches(&move,&arm))
        return 0;
    XenosWindowArm invalid = arm;
    invalid.hash ^= 1;
    XenosFence fence = {.ticket=0};
    UInt32 copied = 0;
    if (XenosCompositeWindowCopy(&screen,&invalid,&fence,&copied) || fence.ticket || copied)
        return 0;
    for (int i = 0; i < kSide*kSide; ++i)
        if (surface[i] != original[i])
            return 0;
    const int passed = XenosCompositeWindowCopy(&screen,&arm,&fence,&copied);
    unsigned guards = 0;
    for (int y = 0; y < kSide; ++y)
        for (int x = 0; x < kSide; ++x)
            if (x < 37 || x >= 37+kWindowCopySide || y < 42 || y >= 42+kWindowCopySide)
                guards += surface[y*kSide+x] == original[y*kSide+x];
    const int ok = passed && guards == kSide*kSide-kWindowCopyPixels;
    printf("XenosWindowCopy %s ticket=%u fence=%u gpu_pixels=%u copied=%u guards=%u hash=%08x\n",
           ok ? "PASS" : "FAIL",fence.ticket,fence.fence,fence.pixels,copied,guards,arm.hash);
    return ok;
}

static int lookup(const API *api)
{
    UInt32 fb = api->matchingService(0,api->serviceMatching("XenonFramebufferTest"));
    void *engine = 0;
    int score = 0, position = 12345;
    int (*allocate)(UInt32, void **, int *) = dlsym(api->io,"IOPSAllocateBlitEngine");
    void (*deallocate)(void *) = dlsym(api->io,"IOPSBlitDeallocate");
    int (*beam)(void *, UInt32, int *) = dlsym(api->io,"IOFBBeamPosition");
    if (!fb || !allocate || !deallocate || !beam)
        return 0;
    const UInt32 status = allocate(fb,&engine,&score);
    UInt32 timing = kBlitNoResources;
    if (!status && engine) {
        timing = beam(engine,0,&position);
        deallocate(engine);
    }
    api->releaseObject(fb);
    printf("XenosWindowRepaint IOPSAllocateBlitEngine=0x%08x score=%d beam=0x%08x\n",
           status,score,timing);
    return !status && engine && timing == kBlitUnsupported && position == 12345;
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1],"--control"))
        return !privateCopyTest();
    UInt32 owner = 0;
    const int baseline = argc == 2 && !strcmp(argv[1],"--baseline");
    const int engine = argc == 2 && !strcmp(argv[1],"--lookup");
    if (!baseline && !engine) {
        if (argc != 3 || strcmp(argv[1],"--repaint"))
            return 2;
        char *end = 0;
        owner = strtoul(argv[2],&end,10);
        if (!owner || !end || *end)
            return 2;
    }
    API api;
    if (!loadAPI(&api))
        return 1;
    return engine ? !lookup(&api) : !windowTest(&api,owner);
}
