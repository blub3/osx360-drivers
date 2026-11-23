#ifndef XENOS_WINDOW_GL_H
#define XENOS_WINDOW_GL_H
#include "Offscreen.h"

enum {
    GL_VIEWPORT = 0x0ba2,
    kAGLSwapRectangle = 200, kCGLEnableSwapRectangle = 201,
    kAGLRGBA = 4, kWindowDrawable = 80, kWindowAttribute = 80, kDoubleBufferAttribute = 5,
    kWindowRenderer = kOffscreenRenderer, kXenosWindowStatsAttribute = 0x58454e57,
    kCGLBadDrawable = 10005
};

typedef struct { float x, y, width, height; } CGRect32;
typedef struct { int x, y, width, height; } RectI;
typedef struct { int connection, window, surface; } WindowSurface;

typedef struct {
    void *library;
    int (*getSurfaceBounds)(int, int, int, CGRect32 *);
    void *(*createWindowContext)(int, int, void *);
    void (*releaseContext)(void *);
    void (*clipToRect)(void *, CGRect32);
    void (*setInterpolationQuality)(void *, int);
    void (*drawImage)(void *, CGRect32, void *);
    void (*flushContext)(void *);
    void *(*createProvider)(void *, const void *, unsigned long, void *);
    void (*releaseProvider)(void *);
    void *(*createRGB)(void);
    void (*releaseColorSpace)(void *);
    void *(*createImage)(unsigned long, unsigned long, unsigned long, unsigned long,
                         unsigned long, void *, unsigned, void *, const float *, int, int);
    void (*releaseImage)(void *);
} XenosPresentation;

int XenosLoadPresentation(XenosPresentation *);
int XenosWindowBounds(const XenosPresentation *, const WindowSurface *, RectI *);
int XenosPresentWindow(const XenosPresentation *, const WindowSurface *, RectI, RectI,
                        const UInt32 *);
#endif
