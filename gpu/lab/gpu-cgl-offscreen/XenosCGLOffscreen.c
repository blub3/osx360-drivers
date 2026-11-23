#include "Offscreen.h"

enum { kSide = kXenosQuadTargetSide, kTextureSide = kXenosQuadUploadSide };
typedef struct { unsigned char red, green, blue, alpha; } Pixel;

static int (*CGLChoosePixelFormat)(const int *, CGLPixelFormatObj *, int *);
static int (*CGLDescribePixelFormat)(CGLPixelFormatObj, int, int, int *);
static int (*CGLDestroyPixelFormat)(CGLPixelFormatObj);
static int (*CGLCreateContext)(CGLPixelFormatObj, CGLContextObj, CGLContextObj *);
static int (*CGLSetOffScreen)(CGLContextObj, int, int, int, void *);
static int (*CGLSetCurrentContext)(CGLContextObj);
static int (*CGLDestroyContext)(CGLContextObj);
static unsigned (*glGetError)(void);
static const unsigned char *(*glGetString)(unsigned);
static void (*glGetIntegerv)(unsigned, int *);
static void (*glViewport)(int, int, int, int);
static void (*glEnable)(unsigned);
static void (*glBindTexture)(unsigned, unsigned);
static void (*glTexParameteri)(unsigned, unsigned, int);
static void (*glTexImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
static void (*glClearColor)(float, float, float, float);
static void (*glClear)(unsigned);
static void (*glBegin)(unsigned);
static void (*glTexCoord2f)(float, float);
static void (*glVertex2f)(float, float);
static void (*glEnd)(void);
static void (*glFinish)(void);
static void (*glReadPixels)(int, int, int, int, unsigned, unsigned, void *);

static int loadAPI(void)
{
    void *cgl = dlopen("/System/Library/Frameworks/OpenGL.framework/Versions/A/OpenGL",
                       kDlopenNow);
    void *gl = dlopen("/System/Library/Frameworks/OpenGL.framework/Versions/A/Libraries/libGL.dylib",
                      kDlopenNow);
    if (!cgl || !gl) {
        printf("XenosCGLOffscreen cannot load Tiger OpenGL: %s\n", dlerror());
        return 0;
    }
#define LOAD(library, name) do { \
    name = dlsym(library, #name); \
    if (!name) { printf("XenosCGLOffscreen missing %s\n", #name); return 0; } \
} while (0)
    LOAD(cgl, CGLChoosePixelFormat);
    LOAD(cgl, CGLDescribePixelFormat);
    LOAD(cgl, CGLDestroyPixelFormat);
    LOAD(cgl, CGLCreateContext);
    LOAD(cgl, CGLSetOffScreen);
    LOAD(gl, CGLSetCurrentContext);
    LOAD(cgl, CGLDestroyContext);
    LOAD(gl, glGetError);
    LOAD(gl, glGetString);
    LOAD(gl, glGetIntegerv);
    LOAD(gl, glViewport);
    LOAD(gl, glEnable);
    LOAD(gl, glBindTexture);
    LOAD(gl, glTexParameteri);
    LOAD(gl, glTexImage2D);
    LOAD(gl, glClearColor);
    LOAD(gl, glClear);
    LOAD(gl, glBegin);
    LOAD(gl, glTexCoord2f);
    LOAD(gl, glVertex2f);
    LOAD(gl, glEnd);
    LOAD(gl, glFinish);
    LOAD(gl, glReadPixels);
#undef LOAD
    return 1;
}

static Pixel texel(unsigned x, unsigned y, unsigned seed)
{
    return (Pixel){(unsigned char)(seed + 17 * x + 3 * y),
                   (unsigned char)(seed * 3 + 5 * x + 13 * y),
                   (unsigned char)(255 - seed - 11 * x - 7 * y), 255};
}

static UInt32 hashPixel(UInt32 hash, Pixel pixel)
{
    hash = xenosQuadHashByte(hash, pixel.red);
    hash = xenosQuadHashByte(hash, pixel.green);
    return xenosQuadHashByte(hash, pixel.blue);
}

static float normalized(int edge)
{
    return 2.0f * edge / kSide - 1.0f;
}

static int checkCGL(const char *operation, int code)
{
    if (!code)
        return 1;
    printf("XenosCGLOffscreen FAIL %s=%d\n", operation, code);
    return 0;
}

static int checkGL(const char *operation)
{
    unsigned code = glGetError();
    if (!code)
        return 1;
    printf("XenosCGLOffscreen FAIL %s GL error=0x%04x\n", operation, code);
    return 0;
}

static int drawFrame(unsigned seed, int left, int bottom, int flip, int legacy,
                     const Pixel *drawable, UInt32 *lastTicket)
{
    static const Pixel clear = {0x4c, 0xb2, 0x37, 0xff};
    unsigned char texture[kTextureSide * kTextureSide * 3];
    Pixel pixels[kSide * kSide];
    for (int y = 0; y < kTextureSide; ++y) {
        for (int x = 0; x < kTextureSide; ++x) {
            const Pixel value = texel(x, y, seed);
            unsigned char *out = texture + 3 * (y * kTextureSide + x);
            out[0] = value.red;
            out[1] = value.green;
            out[2] = value.blue;
        }
    }
    glClearColor(76.0f / 255, 178.0f / 255, 55.0f / 255, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, kTextureSide, kTextureSide, 0, GL_RGB,
                 GL_UNSIGNED_BYTE, texture);
    const float l = normalized(left), r = normalized(left + kTextureSide);
    const float b = normalized(bottom), t = normalized(bottom + kTextureSide);
    glBegin(GL_QUADS);
    glTexCoord2f(flip, 0); glVertex2f(l, b);
    glTexCoord2f(1 - flip, 0); glVertex2f(r, b);
    glTexCoord2f(1 - flip, 1); glVertex2f(r, t);
    glTexCoord2f(flip, 1); glVertex2f(l, t);
    glEnd();
    glFinish();
    if (!checkGL("textured quad/finish"))
        return 0;

    int values[4] = {0};
    glGetIntegerv(kXenosFenceAttribute, values);
    const XenosFence fence = {(UInt32)values[0], (UInt32)values[1],
                              (UInt32)values[2], (UInt32)values[3]};
    if (!checkGL("Xenos fence query") || !fence.ticket || fence.ticket != fence.fence ||
        fence.ticket == *lastTicket || fence.pixels != kXenosQuadReadbackPixels) {
        printf("XenosCGLOffscreen FAIL ticket=%u fence=%u pixels=%u\n",
               fence.ticket, fence.fence, fence.pixels);
        return 0;
    }
    *lastTicket = fence.ticket;

    UInt32 expectedDriverHash = XENOS_QUAD_HASH_INITIAL;
    for (int y = kTextureSide - 1; y >= 0; --y) {
        for (int x = 0; x < kTextureSide; ++x)
            expectedDriverHash = hashPixel(expectedDriverHash,
                texel(flip ? kTextureSide - x - 1 : x, y, seed));
    }
    if (fence.hash != expectedDriverHash) {
        printf("XenosCGLOffscreen FAIL driver hash=%08x expected=%08x\n",
               fence.hash, expectedDriverHash);
        return 0;
    }
    if (legacy) {
        printf("XenosCGLOffscreen FENCE PASS legacy selector seed=%u ticket=%u fence=%u GPU checks=%u hash=%08x\n",
               seed, fence.ticket, fence.fence, fence.pixels, fence.hash);
        return 1;
    }

    glReadPixels(0, 0, kSide, kSide, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    if (!checkGL("glReadPixels"))
        return 0;
    unsigned mismatches = 0, drawableMismatch = 0;
    UInt32 hash = XENOS_QUAD_HASH_INITIAL;
    for (int y = 0; y < kSide; ++y) {
        for (int x = 0; x < kSide; ++x) {
            const int u = x - left, v = y - bottom;
            const int inside = u >= 0 && u < kTextureSide && v >= 0 && v < kTextureSide;
            const Pixel expected = inside ? texel(flip ? kTextureSide - u - 1 : u, v, seed) : clear;
            const unsigned i = y * kSide + x;
            const Pixel actual = pixels[i], attached = drawable[i];
            if (actual.red != expected.red || actual.green != expected.green ||
                actual.blue != expected.blue || actual.alpha != expected.alpha) {
                if (!mismatches)
                    printf("XenosCGLOffscreen first mismatch (%d,%d) got=%02x%02x%02x%02x expected=%02x%02x%02x%02x\n",
                           x, y, actual.red, actual.green, actual.blue, actual.alpha,
                           expected.red, expected.green, expected.blue, expected.alpha);
                ++mismatches;
            }
            drawableMismatch += actual.red != attached.red || actual.green != attached.green ||
                                actual.blue != attached.blue || actual.alpha != attached.alpha;
            hash = hashPixel(hash, actual);
        }
    }
    printf("XenosCGLOffscreen %s seed=%u ticket=%u fence=%u pixels=%u mismatches=%u drawable_mismatches=%u hash=%08x\n",
           mismatches || drawableMismatch ? "FAIL" : "PASS",
           seed, fence.ticket, fence.fence, fence.pixels, mismatches, drawableMismatch, hash);
    return !mismatches && !drawableMismatch;
}

int main(void)
{
    const char *legacyFlag = getenv("XENOS_CGL_LEGACY_FENCE_ONLY");
    const int legacy = legacyFlag && !strcmp(legacyFlag, "1");
    const int attributes[] = {kCGLPFARendererID, kOffscreenRenderer, kOffscreenAttribute,
                              kColorSizeAttribute, 32, kNoRecoveryAttribute, 0};
    CGLPixelFormatObj format = 0;
    CGLContextObj context = 0;
    Pixel drawable[kSide * kSide];
    int renderer = 0, formats = 0, passed = 0;
    if (!loadAPI())
        return 1;
    if (!checkCGL("CGLChoosePixelFormat", CGLChoosePixelFormat(attributes, &format, &formats)))
        goto done;
    if (!format || formats != 1) {
        printf("XenosCGLOffscreen FAIL CGL pixel format count=%d\n", formats);
        goto done;
    }
    if (!checkCGL("CGLDescribePixelFormat",
                  CGLDescribePixelFormat(format, 0, kCGLPFARendererID, &renderer)) ||
        renderer != kOffscreenRenderer) {
        printf("XenosCGLOffscreen FAIL CGL renderer=0x%08x\n", (UInt32)renderer);
        goto done;
    }
    printf("XenosCGLOffscreen CGL pixel format renderer=0x%08x\n", (UInt32)renderer);
    fflush(0);
    if (!checkCGL("CGLCreateContext", CGLCreateContext(format, 0, &context)) ||
        !checkCGL("CGLSetOffScreen", CGLSetOffScreen(context, kSide, kSide, kSide * 4, drawable)) ||
        !checkCGL("CGLSetCurrentContext", CGLSetCurrentContext(context)))
        goto done;
    const unsigned char *name = glGetString(GL_RENDERER);
    if (!name || strcmp((const char *)name, "Xenos offscreen test")) {
        printf("XenosCGLOffscreen FAIL OpenGL renderer=%s\n", name ? (const char *)name : "(null)");
        goto done;
    }
    printf("XenosCGLOffscreen GL_RENDERER=%s\n", name);
    fflush(0);
    glViewport(0, 0, kSide, kSide);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, 1);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    UInt32 previous = 0;
    passed = checkGL("setup") && drawFrame(37, 4, 6, 0, legacy, drawable, &previous) &&
             drawFrame(93, 8, 10, 1, legacy, drawable, &previous);

done:
    if (context) {
        passed &= checkCGL("clear current context", CGLSetCurrentContext(0));
        passed &= checkCGL("destroy context", CGLDestroyContext(context));
    }
    if (format)
        passed &= checkCGL("destroy pixel format", CGLDestroyPixelFormat(format));
    return !passed;
}
