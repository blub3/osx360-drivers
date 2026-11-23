#ifndef XENOS_TIGER_API_H
#define XENOS_TIGER_API_H

/* Public Tiger 32-bit PowerPC API declarations for the cross build. */
typedef unsigned int UInt32;
typedef UInt32 CGDirectDisplayID;
typedef UInt32 io_object_t;
typedef io_object_t io_iterator_t;
typedef io_object_t io_service_t;
typedef struct _CGLRendererInfoObject *CGLRendererInfoObj;
typedef struct _CGLPixelFormatObject *CGLPixelFormatObj;
typedef struct _CGLContextObject *CGLContextObj;

extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
extern int dlclose(void *);
enum { kDlopenNow = 2 };

extern int printf(const char *, ...);
extern int strcmp(const char *, const char *);
extern char *getenv(const char *);

enum {
    kCGLRPRendererID = 70,
    kCGLRPAccelerated = 73,
    kCGLRPDisplayMask = 84,
    kCGLPFARendererID = 70,
    kCGLBadDisplay = 10006,
    kCGLBadMatch = 10009,
    kXenosGLDRendererID = 0xe900,
    kXenosCGLRendererID = 0x00020000 | kXenosGLDRendererID
};

typedef struct {
    CGDirectDisplayID display;
    UInt32 mask;
    unsigned long width;
    unsigned long height;
} XenosDisplay;
int XenosFindDisplay(XenosDisplay *);

#endif
