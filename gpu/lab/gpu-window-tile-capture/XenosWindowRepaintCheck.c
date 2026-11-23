#include "XenosWindowCopy.h"
#include "WindowGL.h"

extern int open(const char *, int, ...);
extern int close(int);
extern long write(int, const void *, unsigned long);
extern int unlink(const char *);
extern unsigned long strtoul(const char *, char **, int);
extern unsigned long long mach_absolute_time(void);
typedef struct { UInt32 numer, denom; } MachTimebase;
extern int mach_timebase_info(MachTimebase *);

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
#ifdef XENOS_UNARMED_BLIT
    void (*selectWindow)(WindowRef);
#endif
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
#ifdef XENOS_UNARMED_BLIT
    LOAD(hit, selectWindow, SelectWindow);
#endif
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

static int paint(const API *api, WindowRef window, short patchX, short patchY)
{
    const RGBColor colors[4] = {{0xf2f2, 0x2626, 0x3636}, {0x1515, 0x9090, 0x4848},
                                {0x2525, 0x4545, 0xc7c7}, {0xcaca, 0xa7a7, 0x1818}};
    void *saved = 0, *port = api->windowPort(window);
    QDRect bounds;
    api->getPortBounds(port, &bounds);
    if (bounds.right - bounds.left < patchX + kWindowCopySide ||
        bounds.bottom - bounds.top < patchY + kWindowCopySide)
        return 0;
    api->getPort(&saved);
    api->setPort(port);
    const RGBColor background = {0x3838, 0x5858, 0x7878};
    api->foreColor(&background);
    api->paintRect(&bounds);
    for (int i = 0; i < 4; ++i) {
        short x = bounds.left + patchX + (i & 1) * (kWindowCopySide/2);
        short y = bounds.top + patchY + (i >> 1) * (kWindowCopySide/2);
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

/* Compare a snapshot of the entire window content, excluding its border. */
static unsigned windowContent(const XenosWindowPixels *screen, int x, int y, int width, int height,
                              UInt32 *pixels, int compare)
{
    if (!screen->base || x < 0 || y < 0 || x + width > (int)screen->width ||
        y + height > (int)screen->height)
        return 0;
    unsigned count = 0;
    for (int line = 0; line < height; ++line) {
        const volatile UInt32 *row = screen->base + (y + line) * (screen->rowBytes / sizeof(UInt32)) + x;
        for (int column = 0; column < width; ++column, ++pixels) {
            const UInt32 rgb = row[column] & 0xffffffU;
            if (compare)
                count += *pixels == rgb;
            else {
                *pixels = rgb;
                ++count;
            }
        }
    }
    return count;
}

enum { kOverlapWidth = 96, kOverlapHeight = 72 };

static int windowTest(const API *api, UInt32 expectedOwner, int overlap, int unarmed)
{
    const int active = expectedOwner != 0;
    const short windowX = overlap ? 216 : 224, windowY = overlap ? 183 : 208;
    const short targetX = overlap ? 244 : 304, targetY = overlap ? 195 : 248;
    const short patchX = overlap ? 24 : 0, patchY = overlap ? 20 : 0;
    const short width = overlap ? kOverlapWidth : kWindowCopySide;
    const short height = overlap ? kOverlapHeight : kWindowCopySide;
    const int x = windowX + patchX, y = windowY + patchY;
    const int destX = targetX + patchX, destY = targetY + patchY;
    const QDRect frame = {windowY, windowX, windowY + height, windowX + width};
    const QDRect backFrame = {160, 182, 256, 310};
    QDRect bounds;
    WindowRef window = 0, back = 0;
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
    if (!screen.base)
        goto done;
    if (overlap) {
        if (api->createWindow(kPlainWindowClass, kWindowNoShadow, &backFrame, &back))
            goto done;
        api->showWindow(back);
        api->runEvents(0.1);
        if (!paint(api, back, 0, 0))
            goto done;
    }
    if (api->createWindow(kPlainWindowClass, kWindowNoShadow, &frame, &window) ||
        api->windowBounds(window, kWindowContent, &bounds) || bounds.left != windowX ||
        bounds.top != windowY || bounds.right != windowX + width || bounds.bottom != windowY + height)
        goto done;
    api->showWindow(window);
#ifdef XENOS_UNARMED_BLIT
    api->selectWindow(window);
#endif
    api->runEvents(0.15);
    if (!paint(api, window, patchX, patchY))
        goto done;
    UInt32 source[kWindowCopyPixels] = {0}, alias[kWindowCopyPixels] = {0}, target[kWindowCopyPixels];
    int havePattern = 0;
    for (int attempt = 0; attempt < 40; ++attempt) {
        if (attempt && attempt % 8 == 0 && !paint(api, window, patchX, patchY))
            goto done;
        api->runEvents(0.05);
        if (XenosWindowSample(&screen,x,y,source) && XenosWindowSample(&mapped,x,y,alias) &&
            matchRGB(source,alias) == kWindowCopyPixels && hasPattern(source)) {
            havePattern = 1;
            break;
        }
    }
    if (!havePattern || !XenosWindowSample(&screen,destX,destY,target) ||
        matchRGB(source,target) == kWindowCopyPixels) {
        printf("XenosWindowRepaint FAIL source pattern=%d alias=%u corners=%08x,%08x,%08x,%08x alias0=%08x\n",
               havePattern, matchRGB(source, alias), source[0], source[8], source[128], source[136], alias[0]);
        goto done;
    }
    UInt32 contents[kOverlapWidth * kOverlapHeight];
    const unsigned contentPixels = width * height;
    if (windowContent(&screen, windowX, windowY, width, height, contents, 0) != contentPixels)
        goto done;
    const XenosWindowArm arm = {kGPUWindowCopyMagic, {x, y, destX, destY}, XenosWindowHash(source)};
    XenosGPUCompletion before = {.sequence=0}, after = {.sequence=0};
    const int haveBefore = completion(api, gpu, &before);
    MachTimebase timebase;
    if (!haveBefore || mach_timebase_info(&timebase) || !timebase.numer || !timebase.denom ||
        (active && !unarmed && !(armed=armCopy(&arm))))
        goto done;
    /* The asynchronous move may take the plug-in's full eight-second GPU budget. */
    const unsigned long long waitNs = unarmed ? 2000000000ULL
        : active && overlap ? 12000000000ULL : 1250000000ULL;
    const unsigned long long moveStart = mach_absolute_time();
    const unsigned long long deadline = moveStart + waitNs * timebase.denom / timebase.numer;
    api->moveWindow(window, targetX, targetY, 1);
    unsigned matches = 0, contentMatches = 0;
    do {
        api->runEvents(0.05);
        if (XenosWindowSample(&screen,destX,destY,target))
            matches = matchRGB(source,target);
        contentMatches = windowContent(&screen, targetX, targetY, width, height, contents, 1);
        if (matches == kWindowCopyPixels && contentMatches == contentPixels)
            break;
        usleep(10000);
    } while (mach_absolute_time() < deadline);
    const unsigned long long moveTicks = mach_absolute_time() - moveStart;
    const UInt32 moveMs = (UInt32)(moveTicks * timebase.numer / timebase.denom / 1000000ULL);
    const int atTarget = !api->windowBounds(window,kWindowContent,&bounds) &&
        bounds.left == targetX && bounds.top == targetY;
    const int readCompletion = completion(api,gpu,&after);
    ok = atTarget && matches == kWindowCopyPixels && contentMatches == contentPixels && readCompletion &&
        (active ? after.sequence != before.sequence && after.ownerPID == expectedOwner &&
                  after.ticket && after.fence == after.ticket && after.state == kXenosQuadPassed &&
                  after.pixels == kXenosQuadReadbackPixels && !after.mismatches :
                  after.sequence == before.sequence);
    printf("XenosWindowRepaint %s mode=%s src=%d,%d dst=%d,%d hash=%08x "
           "screen_pixels=%u owner_pid=%u ticket=%u fence=%u gpu_pixels=%u gpu_mismatches=%u "
           "QuartzExtreme_before=%d after=%d window_pixels=%u/%u move_ms=%u\n",
           ok ? "PASS" : "FAIL", unarmed ? "unarmed" : active ? (overlap ? "overlap" : "repaint") : (overlap ? "overlap-baseline" : "baseline"),
           x, y, destX, destY, arm.hash,
           matches, after.ownerPID, after.ticket, after.fence, after.pixels, after.mismatches,
           qeBefore, api->qe(display.display) != 0, contentMatches, contentPixels, moveMs);
done:
    if (armed)
        unlink(XENOS_WINDOW_ARM);
    if (window)
        api->disposeWindow(window);
    if (back)
        api->disposeWindow(back);
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
    enum { kSide = 128, kRegionPixels = 7252, kExpectedBatches = 8, kShift = 11 };
    UInt32 surface[kSide * kSide], original[kSide * kSide];
    const XenosWindowPixels screen = {surface, kSide * sizeof(UInt32), kSide, kSide};
    XenosWindowArm arm = {kGPUWindowCopyMagic, {61, 15, 72, 26}, 0};
    for (int y = 0; y < kSide; ++y)
        for (int x = 0; x < kSide; ++x)
            surface[y*kSide + x] = (UInt32)((x*9 + y*33) & 255) << 24 |
                (UInt32)((x*37 + y*11) & 255) << 16 |
                (UInt32)((x*13 + y*107) & 255) << 8 | (UInt32)((x*83 + y*19) & 255);
    memcpy(original, surface, sizeof(surface));
    UInt32 source[kWindowCopyPixels];
    if (!XenosWindowSample(&screen, arm.move.sourceX, arm.move.sourceY, source))
        return 0;
    arm.hash = XenosWindowHash(source);
    /* Two clipped rectangles straddle the verification tile and overlap their destination. */
    const struct { UInt32 op[22], count; TigerCopyRect rects[2]; } rectangle =
        {{0}, 2, {{6,8,17,19,62,74}, {68,10,79,21,36,74}}};
    const struct { UInt32 count; short bounds[4], rect[2][4]; } region =
        {2, {6,8,98,76}, {{6,8,62,74}, {68,10,36,74}}};
    const TigerCopyRegion regionCopy = {.deltaX=17, .deltaY=19, .region=&region};
    XenosCopyPlan plan;
    if (!XenosDecodeWindowCopy(kBlitCopy, 0, &rectangle, &plan) ||
        !XenosWindowCopyCovers(&plan, &arm) || plan.area != kRegionPixels ||
        !XenosDecodeWindowCopy(kBlitCopyRegion, kBlitFramebuffer, &regionCopy, &plan) ||
        !XenosWindowCopyCovers(&plan, &arm) || plan.area != kRegionPixels)
        return 0;
    XenosFullCopy copy;
    const struct { UInt32 op[22], count; TigerCopyRect rects[1]; } oversize =
        {{0}, 1, {{6, 8, 17, 19, 98, 96}}};
    XenosCopyPlan overBudget;
    if (!XenosDecodeWindowCopy(kBlitCopy, 0, &oversize, &overBudget))
        return 0;
    const char *failure = XenosCompositeWindowCopy(&screen, &overBudget, &arm, &copy);
    if (!failure || strcmp(failure, "region-limit") || copy.batches || copy.committed)
        return 0;
    XenosWindowArm invalid = arm;
    invalid.hash ^= 1;
    failure = XenosCompositeWindowCopy(&screen, &plan, &invalid, &copy);
    if (!failure || strcmp(failure, "source-hash") || copy.batches || copy.committed)
        return 0;
    for (int i = 0; i < kSide*kSide; ++i)
        if (surface[i] != original[i])
            return 0;
    failure = XenosCompositeWindowCopy(&screen, &plan, &arm, &copy);
    unsigned guards = 0, regionPixels = 0;
    for (int y = 0; y < kSide; ++y) {
        for (int x = 0; x < kSide; ++x) {
            const int inCopy = (x >= 17 && x < 79 && y >= 19 && y < 93) ||
                               (x >= 79 && x < 115 && y >= 21 && y < 95);
            if (inCopy)
                regionPixels += surface[y*kSide+x] == original[(y-kShift)*kSide+x-kShift];
            else
                guards += surface[y*kSide+x] == original[y*kSide+x];
        }
    }
    const int ok = !failure && copy.committed && copy.batches == kExpectedBatches &&
        copy.fence - copy.firstFence + 1 == copy.batches && copy.gpuPixels == kRegionPixels &&
        copy.screenPixels == kRegionPixels && regionPixels == kRegionPixels && guards == kSide*kSide-kRegionPixels;
    char fences[kBatchFenceTextBytes];
    XenosFormatBatchFences(&copy, fences);
    printf("XenosBatchedCopy %s batches=%u first_fence=%u ticket=%u fence=%u gpu_pixels=%u "
           "screen_pixels=%u region=%u guards=%u hash=%08x gpu_ms=%u fences=%s reason=%s\n",
           ok ? "PASS" : "FAIL", copy.batches, copy.firstFence, copy.ticket, copy.fence,
           copy.gpuPixels, copy.screenPixels, regionPixels, guards, copy.hash, copy.elapsedMs,
           fences, failure ? failure : "verified");
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

static int mappingPolicy(const API *api, int previous)
{
    enum { kNotPrivileged = 0xe00002c1U, kBadArgument = 0xe00002c2U };
    UInt32 fb = api->matchingService(0, api->serviceMatching("XenonFramebufferTest"));
    XenosFramebufferMap map;
    UInt32 status = 0;
    if (!fb)
        return 0;
    const char *reason = XenosMapWindowAperture(api->io, fb, &map, &status);
    if (!reason)
        XenosUnmapFramebuffer(&map);
    api->releaseObject(fb);
    const int denied = reason && !strcmp(reason, "map-open") &&
                       status == (previous ? kBadArgument : kNotPrivileged);
    printf("XenosWindowAperture %s private map for non-owner stage=%s status=0x%08x\n",
           denied ? "PASS" : "FAIL", reason ? reason : "mapped", status);
    return denied;
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1],"--control"))
        return !privateCopyTest();
    UInt32 owner = 0;
    const int unarmed = argc == 3 && !strcmp(argv[1], "--unarmed");
    const int overlap = argc > 1 && (!strcmp(argv[1], "--overlap") || !strcmp(argv[1], "--overlap-baseline"));
    const int baseline = argc == 2 && (!strcmp(argv[1], "--baseline") || !strcmp(argv[1], "--overlap-baseline"));
    const int engine = argc == 2 && !strcmp(argv[1],"--lookup");
    const int policy = argc == 2 && (!strcmp(argv[1], "--mapping-policy") ||
                                     !strcmp(argv[1], "--mapping-baseline"));
    if (!baseline && !engine && !policy) {
        if (argc != 3 || (!unarmed && strcmp(argv[1], "--repaint") && strcmp(argv[1], "--overlap")))
            return 2;
        char *end = 0;
        owner = strtoul(argv[2],&end,10);
        if (!owner || !end || *end)
            return 2;
    }
    API api;
    if (!loadAPI(&api))
        return 1;
    if (policy)
        return !mappingPolicy(&api, !strcmp(argv[1], "--mapping-baseline"));
    return engine ? !lookup(&api) : !windowTest(&api, owner, overlap, unarmed);
}
