#ifndef XENOS_OFFSCREEN_H
#define XENOS_OFFSCREEN_H
#include "TigerAPI.h"
#include "XenosQuadProtocol.h"

extern void *calloc(unsigned long, unsigned long);
extern void free(void *);
extern void *memcpy(void *, const void *, unsigned long);
extern int usleep(unsigned);
extern int fflush(void *);
extern char *dlerror(void);

enum {
    GL_QUADS = 7, GL_RGBA = 0x1908, GL_RGB = 0x1907, GL_UNSIGNED_BYTE = 0x1401,
    GL_TEXTURE_2D = 0x0de1, GL_TEXTURE_MIN_FILTER = 0x2801, GL_TEXTURE_MAG_FILTER = 0x2800,
    GL_NEAREST = 0x2600, GL_RENDERER = 0x1f01, GL_COLOR_BUFFER_BIT = 0x4000,
    GL_INVALID_ENUM = 0x500, GL_INVALID_OPERATION = 0x502,
    kOffscreenRenderer = 0x0004e900, kOffscreenPlugin = 0x00040000,
    kOffscreenDrawable = 0x35, kOffscreenAttribute = 53, kColorSizeAttribute = 8, kNoRecoveryAttribute = 72,
    kCGLBadPixelFormat = 10002, kCGLBadOffScreen = 10011, kCGLBadAlloc = 10016,
    kBackground = 0xff4cb237U,
};

typedef struct {
    UInt32 ticket, fence, pixels, hash;
} XenosFence;
enum { kXenosFenceAttribute = 0x58454e43 };

typedef struct {
    void *library;
    UInt32 connection;
    int (*IOServiceClose)(UInt32);
    int (*IOConnectMethodStructureIStructureO)(UInt32, unsigned, unsigned long,
                                               unsigned long *, const void *, void *);
    int (*IOConnectMethodScalarIScalarO)(UInt32, unsigned, unsigned, unsigned, ...);
} XenosBackend;
int XenosOpenBackend(XenosBackend *);
void XenosCloseBackend(XenosBackend *);
int XenosRenderQuad(XenosBackend *, const XenosQuadGeometryUpload *, int legacy,
                   XenosFence *, UInt32 *argb);
#endif
