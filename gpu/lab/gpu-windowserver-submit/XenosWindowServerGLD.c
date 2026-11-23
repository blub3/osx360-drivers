#include "Offscreen.h"

extern int getpid(void);
int XenosIsWindowServer(void);
void XenosProbeWindowServerStartup(void);

/* Renderer record read by Tiger's stock GLEngine. */
typedef struct {
    UInt32 next, rendererID, flags, bufferModes, colorModes;
    unsigned char reserved[36];
} TigerRendererInfo;
typedef char TigerRendererInfoSize[sizeof(TigerRendererInfo) == 56 ? 1 : -1];

static int runFence(void)
{
    const UInt32 left = 5, top = 7, side = 16;
    XenosQuadGeometryUpload upload;
    upload.geometry = xenosQuadMakeGeometry(left, top, left + side, top + side, 0, 0, side, side);
    for (unsigned i = 0; i < kXenosQuadTexturePixels; ++i) {
        upload.rgb[3*i] = (unsigned char)(19 + i);
        upload.rgb[3*i+1] = (unsigned char)(201 - i);
        upload.rgb[3*i+2] = (unsigned char)(71 + 9*i);
    }

    XenosBackend backend = {.connection = 0};
    if (!XenosOpenBackend(&backend))
        return 0;
    XenosFence fence;
    UInt32 pixels[kXenosQuadReadbackPixels];
    int complete = XenosRenderQuad(&backend, &upload, 0, &fence, pixels);
    XenosCloseBackend(&backend);
    if (!complete)
        return 0;

    unsigned mismatches = 0;
    for (UInt32 y = 0; y < kXenosQuadTargetSide; ++y) {
        for (UInt32 x = 0; x < kXenosQuadTargetSide; ++x) {
            UInt32 expected = kBackground;
            if (x >= left && x < left + side && y >= top && y < top + side) {
                UInt32 i = (y - top)*side + x - left;
                expected = 0xff000000U | ((UInt32)upload.rgb[3*i] << 16) |
                           ((UInt32)upload.rgb[3*i+1] << 8) | upload.rgb[3*i+2];
            }
            mismatches += pixels[y*kXenosQuadTargetSide + x] != expected;
        }
    }
    printf("XenosNativeGLD gldGetRendererInfo pid=%d ticket=%u fence=%u pixels=%u mismatches=%u checksum=%08x\n",
           getpid(), fence.ticket, fence.fence, fence.pixels, mismatches, fence.hash);
    return mismatches == 0;
}

void gldInitializeLibrary(void)
{
    XenosProbeWindowServerStartup();
}

int gldGetVersion(int *major, int *minor, int *revision, int *renderer)
{
    *major = 2;
    *minor = 4;
    *revision = 11;
    *renderer = kXenosGLDRendererID;
    return 1;
}

int gldGetRendererInfo(TigerRendererInfo *info, UInt32 mask)
{
    // CoreGraphics is using GLEngine inside the server during this startup pass.
    if (XenosIsWindowServer())
        return kCGLBadMatch;
    XenosDisplay display;
    if (!info || !XenosFindDisplay(&display) || mask != display.mask)
        return kCGLBadMatch;

    /* Only the single-threaded diagnostic arms this one-shot submission. */
    const char *armed = getenv("XENOS_NATIVE_GLD_PROBE");
    static int submitted;
    if (armed && !strcmp(armed, "1") && !submitted) {
        submitted = 1;
        if (!runFence()) {
            printf("XenosNativeGLD FAIL GLD fence or pixel readback\n");
            return kCGLBadMatch;
        }
    }
    *info = (TigerRendererInfo){.rendererID = kXenosGLDRendererID};
    return 0;
}

int gldChoosePixelFormat(void **format, const int *attributes)
{
    (void)attributes;
    XenosProbeWindowServerStartup();
    if (!format)
        return kCGLBadMatch;
    *format = 0;
    return 0;
}

#define GLD_UNSUPPORTED(name) int name(void) { return kCGLBadMatch; }
#include "UnsupportedGLD.def"
