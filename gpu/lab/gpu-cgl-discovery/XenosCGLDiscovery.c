#include "TigerAPI.h"

int main(void)
{
    const char *mode = getenv("XENOS_CGL_DISCOVERY");
    int expectXenos = mode && !strcmp(mode, "enabled");
    int count = 0;
    int matches = 0;
    int badProperties = 0;
    int nativeRenderers = 0;
    CGLRendererInfoObj info = 0;
    XenosDisplay display;

    if (!XenosFindDisplay(&display)) {
        printf("XenosCGLDiscovery FAIL no active Xenos framebuffer and GPU pair\n");
        return 1;
    }
    printf("XenosCGLDiscovery display=0x%08x mask=0x%08x mode=%s dimensions=%lux%lu\n",
           display.display, display.mask, expectXenos ? "private" : "baseline",
           display.width, display.height);

    void *openGL = dlopen("/System/Library/Frameworks/OpenGL.framework/Versions/A/OpenGL",
                          kDlopenNow);
    if (!openGL) {
        printf("XenosCGLDiscovery FAIL cannot load Tiger OpenGL.framework\n");
        return 1;
    }
    int (*CGLQueryRendererInfo)(UInt32, CGLRendererInfoObj *, int *) =
        dlsym(openGL, "CGLQueryRendererInfo");
    int (*CGLDescribeRenderer)(CGLRendererInfoObj, int, int, int *) =
        dlsym(openGL, "CGLDescribeRenderer");
    int (*CGLDestroyRendererInfo)(CGLRendererInfoObj) =
        dlsym(openGL, "CGLDestroyRendererInfo");
    int (*CGLChoosePixelFormat)(const int *, CGLPixelFormatObj *, int *) =
        dlsym(openGL, "CGLChoosePixelFormat");
    int (*CGLCreateContext)(CGLPixelFormatObj, CGLContextObj, CGLContextObj *) =
        dlsym(openGL, "CGLCreateContext");
    int (*CGLDestroyContext)(CGLContextObj) = dlsym(openGL, "CGLDestroyContext");
    int (*CGLDestroyPixelFormat)(CGLPixelFormatObj) = dlsym(openGL, "CGLDestroyPixelFormat");
    if (!CGLQueryRendererInfo || !CGLDescribeRenderer || !CGLDestroyRendererInfo ||
        !CGLChoosePixelFormat || !CGLCreateContext || !CGLDestroyContext ||
        !CGLDestroyPixelFormat) {
        printf("XenosCGLDiscovery FAIL missing public CGL symbol\n");
        return 1;
    }

    int status = CGLQueryRendererInfo(display.mask, &info, &count);
    if (status || !info || count <= 0) {
        printf("XenosCGLDiscovery FAIL CGLQueryRendererInfo=%d count=%d\n", status, count);
        if (info)
            CGLDestroyRendererInfo(info);
        return 1;
    }
    for (int i = 0; i < count; ++i) {
        int id = 0, accelerated = -1, mask = 0;
        if (CGLDescribeRenderer(info, i, kCGLRPRendererID, &id) ||
            CGLDescribeRenderer(info, i, kCGLRPAccelerated, &accelerated) ||
            CGLDescribeRenderer(info, i, kCGLRPDisplayMask, &mask)) {
            badProperties = 1;
            break;
        }
        printf("renderer[%d] id=0x%08x accelerated=%d displayMask=0x%08x\n",
               i, (UInt32)id, accelerated, (UInt32)mask);
        if (id == kXenosCGLRendererID) {
            ++matches;
            badProperties |= accelerated != 0 || (UInt32)mask != display.mask;
        } else {
            ++nativeRenderers;
        }
    }
    status = CGLDestroyRendererInfo(info);
    if (status || matches != expectXenos || !nativeRenderers || badProperties) {
        printf("XenosCGLDiscovery FAIL matches=%d expected=%d native=%d properties=%d destroy=%d\n",
               matches, expectXenos, nativeRenderers, badProperties, status);
        return 1;
    }
    if (expectXenos) {
        const int attrs[] = {kCGLPFARendererID, kXenosCGLRendererID, 0};
        CGLPixelFormatObj format = 0;
        CGLContextObj context = 0;
        int formats = 0;
        int choose = CGLChoosePixelFormat(attrs, &format, &formats);
        int create = choose || !format ? 0 : CGLCreateContext(format, 0, &context);
        printf("XenosCGLDiscovery context guard choose=%d formats=%d create=%d\n",
               choose, formats, create);
        if (format)
            CGLDestroyPixelFormat(format);
        if (context)
            CGLDestroyContext(context);
        if (context || (!choose && format && !create)) {
            printf("XenosCGLDiscovery FAIL diagnostic renderer created a context\n");
            return 1;
        }
    }
    printf("XenosCGLDiscovery PASS mode=%s count=%d native=%d\n",
           expectXenos ? "private" : "baseline", count, nativeRenderers);
    return 0;
}
