#include "Offscreen.h"
#include "XenosDisplaySurfaceProtocol.h"

extern int _NSGetExecutablePath(char *, UInt32 *);
extern char *realpath(const char *, char *);
extern void syslog(int, const char *, ...);

typedef struct {
    UInt32 count;
    short x, y, w, h;
} TigerDeviceRegion;

enum {
    kLogNotice = 5,
    kSurfaceModeARGBWindowed = 0x24,
    kBusy = 0xe00002d5U,
    kNotReady = 0xe00002d8U,
};

int XenosIsWindowServer(void)
{
    static int detected;
    if (!detected) {
        static const char stock[] = "/System/Library/Frameworks/ApplicationServices.framework/"
            "Frameworks/CoreGraphics.framework/Resources/WindowServer";
        char executable[1024], actual[1024], expected[1024];
        UInt32 size = sizeof(executable);
        detected = !_NSGetExecutablePath(executable, &size) &&
                   realpath(executable, actual) && realpath(stock, expected) &&
                   !strcmp(actual, expected) ? 1 : -1;
    }
    return detected == 1;
}

void XenosProbeWindowServerStartup(void)
{
    static int attempted;
    if (attempted || !XenosIsWindowServer())
        return;
    attempted = 1;

    /* The server is in CGL initialization. Use only IOKit here. */
    void *io = dlopen("/System/Library/Frameworks/IOKit.framework/Versions/A/IOKit", kDlopenNow);
    if (!io)
        return;
    UInt32 (*matchingService)(UInt32, void *) = dlsym(io, "IOServiceGetMatchingService");
    void *(*matching)(const char *) = dlsym(io, "IOServiceMatching");
    int (*release)(UInt32) = dlsym(io, "IOObjectRelease");
    int (*create)(UInt32, UInt32, UInt32, UInt32 *) = dlsym(io, "IOAccelCreateSurface");
    int (*shape)(UInt32, const TigerDeviceRegion *, UInt32, UInt32) =
        dlsym(io, "IOAccelSetSurfaceFramebufferShape");
    int (*flush)(UInt32, UInt32, UInt32) = dlsym(io, "IOAccelFlushSurfaceOnFramebuffers");
    int (*readLock)(UInt32, XenosTigerSurfaceInfo *, UInt32) = dlsym(io, "IOAccelReadLockSurface");
    int (*unlock)(UInt32) = dlsym(io, "IOAccelReadUnlockSurface");
    int (*destroy)(UInt32) = dlsym(io, "IOAccelDestroySurface");

    UInt32 accelerator = 0, surface = 0;
    unsigned clears = 0;
    int status = -1;
    const char *step = "IOKit functions";
    if (!matchingService || !matching || !release || !create || !shape || !flush ||
        !readLock || !unlock || !destroy)
        goto done;

    step = "find Xenos accelerator";
    accelerator = matchingService(0, matching("XenosDisplayAccelerator"));
    if (!accelerator)
        goto done;

    step = "create surface";
    status = create(accelerator, kXenosDiagnosticSurfaceID, kSurfaceModeARGBWindowed, &surface);
    if (status || !surface)
        goto done;

    step = "set shape";
    const TigerDeviceRegion bounds = {0, 0, 0, kXenosSurfaceSide, kXenosSurfaceSide};
    status = shape(surface, &bounds, 0, 0);
    if (status)
        goto done;

    const UInt32 colors[] = {kXenosDiagnosticSurfaceFirst, kXenosDiagnosticSurfaceSecond};
    for (; clears < sizeof(colors) / sizeof(colors[0]); ++clears) {
        step = "flush surface";
        status = (int)kBusy;
        for (unsigned attempt = 0; attempt < 80 && (UInt32)status == kBusy; ++attempt) {
            status = flush(surface, 0, 1);
            if ((UInt32)status == kBusy)
                usleep(10000);
        }
        if (status)
            goto done;

        step = "read-lock surface";
        XenosTigerSurfaceInfo info;
        status = (int)kNotReady;
        for (unsigned attempt = 0; attempt < 120 && (UInt32)status == kNotReady; ++attempt) {
            status = readLock(surface, &info, sizeof(info));
            if ((UInt32)status == kNotReady)
                usleep(10000);
        }
        if (status)
            goto done;

        step = "verify surface pixels";
        UInt32 mismatches = 1;
        if (info.address[0] && !(info.address[0] & 4095U) &&
            info.width == kXenosSurfaceSide && info.height == kXenosSurfaceSide &&
            info.rowBytes == kXenosSurfaceSide * sizeof(UInt32) &&
            info.pixelFormat == kXenosDiagnosticSurfaceFormat) {
            mismatches = 0;
            const UInt32 *pixels = (const UInt32 *)info.address[0];
            for (unsigned i = 0; i < kXenosSurfaceSide * kXenosSurfaceSide; ++i)
                mismatches += pixels[i] != colors[clears];
        }
        status = unlock(surface);
        if (status)
            goto done;
        if (mismatches) {
            status = -1;
            goto done;
        }
    }

    step = "destroy surface";
    status = destroy(surface);
    surface = 0;

done:
    if (surface)
        destroy(surface);
    if (accelerator)
        release(accelerator);
    if (status || clears != kXenosWSStartupClears)
        syslog(kLogNotice, "XenosWindowServerStartup FAIL %s status=0x%08x clears=%u", step,
               (UInt32)status, clears);
    else
        syslog(kLogNotice, "XenosWindowServerStartup PASS clears=%u readback_pixels=%u",
               clears, clears * kXenosSurfaceSide * kXenosSurfaceSide);
    dlclose(io);
}
