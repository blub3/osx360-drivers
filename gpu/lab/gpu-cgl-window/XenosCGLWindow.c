#include "WindowGL.h"

enum { kSide = kXenosQuadTargetSide, kTex = kXenosQuadUploadSide, kSceneX = 220, kSceneY = 180,
       kPlainWindowClass = 13, kWindowNoShadow = 1U << 21, kWindowContentRegion = 33 };
typedef void *WindowRef;
typedef void *AGLPixelFormat;
typedef void *AGLContext;
typedef const unsigned char *(*GetString)(unsigned);
typedef struct { short top, left, bottom, right; } QDRect;

static int (*CreateNewWindow)(unsigned, unsigned, const QDRect *, WindowRef *);
static void (*DisposeWindow)(WindowRef);
static void (*ShowWindow)(WindowRef);
static void *(*GetWindowPort)(WindowRef);
static int (*GetWindowBounds)(WindowRef, unsigned, QDRect *);
static void (*SizeWindow)(WindowRef, short, short, unsigned char);
static void (*RunCurrentEventLoop)(double);
static AGLPixelFormat (*aglChoosePixelFormat)(const void *, int, const int *);
static unsigned char (*aglDescribePixelFormat)(AGLPixelFormat, int, int *);
static void (*aglDestroyPixelFormat)(AGLPixelFormat);
static AGLContext (*aglCreateContext)(AGLPixelFormat, AGLContext);
static unsigned char (*aglSetDrawable)(AGLContext, void *);
static unsigned char (*aglSetCurrentContext)(AGLContext);
static unsigned char (*aglUpdateContext)(AGLContext);
static unsigned char (*aglSetInteger)(AGLContext, unsigned, const int *);
static unsigned char (*aglEnable)(AGLContext, unsigned);
static void (*aglSwapBuffers)(AGLContext);
static unsigned char (*aglDestroyContext)(AGLContext);
static unsigned (*aglGetError)(void);
static unsigned (*glGetError)(void);
static GetString glGetString;
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
static CGDirectDisplayID (*CGMainDisplayID)(void);
static void *(*CGDisplayBaseAddress)(CGDirectDisplayID);
static unsigned long (*CGDisplayBytesPerRow)(CGDirectDisplayID);
static unsigned long (*CGDisplayBitsPerPixel)(CGDirectDisplayID);
static unsigned long (*CGDisplayPixelsWide)(CGDirectDisplayID);
static unsigned long (*CGDisplayPixelsHigh)(CGDirectDisplayID);

static int loadAPI(void)
{
    void *hit = dlopen("/System/Library/Frameworks/Carbon.framework/Versions/A/Frameworks/"
                       "HIToolbox.framework/Versions/A/HIToolbox", kDlopenNow);
    void *agl = dlopen("/System/Library/Frameworks/AGL.framework/Versions/A/AGL", kDlopenNow);
    void *gl = dlopen("/System/Library/Frameworks/OpenGL.framework/Versions/A/Libraries/libGL.dylib",
                      kDlopenNow);
    void *cg = dlopen("/System/Library/Frameworks/ApplicationServices.framework/Versions/A/"
                      "Frameworks/CoreGraphics.framework/Versions/A/CoreGraphics", kDlopenNow);
    if (!hit || !agl || !gl || !cg) {
        printf("XenosCGLWindow FAIL loading Tiger frameworks: %s\n", dlerror());
        return 0;
    }
#define LOAD(library, name) do { \
    name = dlsym(library, #name); \
    if (!name) { printf("XenosCGLWindow missing %s\n", #name); return 0; } \
} while (0)
    LOAD(hit, CreateNewWindow);
    LOAD(hit, DisposeWindow);
    LOAD(hit, ShowWindow);
    LOAD(hit, GetWindowPort);
    LOAD(hit, GetWindowBounds);
    LOAD(hit, SizeWindow);
    LOAD(hit, RunCurrentEventLoop);
    LOAD(agl, aglChoosePixelFormat);
    LOAD(agl, aglDescribePixelFormat);
    LOAD(agl, aglDestroyPixelFormat);
    LOAD(agl, aglCreateContext);
    LOAD(agl, aglSetDrawable);
    LOAD(agl, aglSetCurrentContext);
    LOAD(agl, aglUpdateContext);
    LOAD(agl, aglSetInteger);
    LOAD(agl, aglEnable);
    LOAD(agl, aglSwapBuffers);
    LOAD(agl, aglDestroyContext);
    LOAD(agl, aglGetError);
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
    LOAD(cg, CGMainDisplayID);
    LOAD(cg, CGDisplayBaseAddress);
    LOAD(cg, CGDisplayBytesPerRow);
    LOAD(cg, CGDisplayBitsPerPixel);
    LOAD(cg, CGDisplayPixelsWide);
    LOAD(cg, CGDisplayPixelsHigh);
#undef LOAD
    return 1;
}

typedef struct { unsigned seed; int left, bottom, flip; } Frame;

static UInt32 texel(int x, int y, unsigned seed)
{
    return 0xff000000U | ((seed + 17*x + 3*y) & 255U) << 16 |
           ((seed*3 + 5*x + 13*y) & 255U) << 8 |
           ((255 - seed - 11*x - 7*y) & 255U);
}

static UInt32 framePixel(Frame frame, int x, int y)
{
    int u = x - frame.left, v = y - frame.bottom;
    if (u < 0 || u >= kTex || v < 0 || v >= kTex)
        return kBackground;
    return texel(frame.flip ? kTex - u - 1 : u, v, frame.seed);
}

static float normalized(int edge) { return 2.0f * edge / kSide - 1.0f; }

static int checkGL(const char *operation)
{
    unsigned code = glGetError();
    if (!code)
        return 1;
    printf("XenosCGLWindow FAIL %s GL error=0x%04x\n", operation, code);
    return 0;
}

static int draw(Frame frame, UInt32 *ticketOut)
{
    unsigned char texture[kTex * kTex * 3];
    unsigned char pixels[kSide * kSide * 4];
    for (int y = 0; y < kTex; ++y) {
        for (int x = 0; x < kTex; ++x) {
            UInt32 value = texel(x, y, frame.seed);
            unsigned char *out = texture + 3 * (y * kTex + x);
            out[0] = value >> 16;
            out[1] = value >> 8;
            out[2] = value;
        }
    }
    glClearColor(76.0f / 255, 178.0f / 255, 55.0f / 255, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, kTex, kTex, 0, GL_RGB, GL_UNSIGNED_BYTE, texture);
    const float l = normalized(frame.left), r = normalized(frame.left + kTex);
    const float b = normalized(frame.bottom), t = normalized(frame.bottom + kTex);
    glBegin(GL_QUADS);
    glTexCoord2f(frame.flip, 0); glVertex2f(l, b);
    glTexCoord2f(1 - frame.flip, 0); glVertex2f(r, b);
    glTexCoord2f(1 - frame.flip, 1); glVertex2f(r, t);
    glTexCoord2f(frame.flip, 1); glVertex2f(l, t);
    glEnd();
    glFinish();
    if (!checkGL("draw/finish"))
        return 0;
    int fence[4] = {0};
    glGetIntegerv(kXenosFenceAttribute, fence);
    if (!checkGL("fence") || !fence[0] || fence[0] != fence[1] ||
        fence[2] != kSide * kSide || (UInt32)fence[0] == *ticketOut) {
        printf("XenosCGLWindow FAIL ticket=%u fence=%u checked=%d\n", (UInt32)fence[0], (UInt32)fence[1], fence[2]);
        return 0;
    }
    *ticketOut = fence[0];
    glReadPixels(0, 0, kSide, kSide, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    if (!checkGL("glReadPixels"))
        return 0;
    unsigned mismatches = 0;
    for (int y = 0; y < kSide; ++y) {
        for (int x = 0; x < kSide; ++x) {
            const unsigned char *p = pixels + 4 * (y * kSide + x);
            const UInt32 actual = (UInt32)p[3] << 24 | (UInt32)p[0] << 16 | (UInt32)p[1] << 8 | p[2];
            mismatches += actual != framePixel(frame, x, y);
        }
    }
    printf("XenosCGLWindow %s draw seed=%u ticket=%u fence=%u back_pixels=%u mismatches=%u\n",
           mismatches ? "FAIL" : "PASS", frame.seed, *ticketOut, (UInt32)fence[1], kSide*kSide, mismatches);
    fflush(0);
    return !mismatches;
}

static int screenCheck(WindowRef window, const char *label, Frame front, const Frame *prior,
                       RectI clip)
{
    QDRect bounds;
    if (GetWindowBounds(window, kWindowContentRegion, &bounds))
        return 0;
    const int width = bounds.right - bounds.left, height = bounds.bottom - bounds.top;
    CGDirectDisplayID display = CGMainDisplayID();
    const unsigned long stride = CGDisplayBytesPerRow(display);
    const volatile unsigned char *address = CGDisplayBaseAddress(display);
    if (!address || CGDisplayBitsPerPixel(display) != 32 || bounds.top < 0 || bounds.left < 0 ||
        bounds.right > (int)CGDisplayPixelsWide(display) ||
        bounds.bottom > (int)CGDisplayPixelsHigh(display) || width <= 0 || height <= 0 ||
        width % kSide || height % kSide || stride < (unsigned)bounds.right * 4) {
        printf("XenosCGLWindow FAIL cannot read window rectangle from display addr=%p size=%dx%d\n",
               (const void *)address, width, height);
        return 0;
    }
    unsigned mismatches = 0, insideCount = 0, outsideCount = 0;
    unsigned changedInside = 0, changedOutside = 0;
    for (unsigned attempt = 0; attempt < 12; ++attempt) {
        RunCurrentEventLoop(0.07);
        mismatches = insideCount = outsideCount = changedInside = changedOutside = 0;
        for (int y = 0; y < height; ++y) {
            const volatile UInt32 *row = (const volatile UInt32 *)(address + stride * (bounds.bottom - y - 1));
            for (int x = 0; x < width; ++x) {
                const int inside = x >= clip.x && y >= clip.y &&
                                   x < clip.x + clip.width && y < clip.y + clip.height;
                const int tx = x / (width / kSide), ty = y / (height / kSide);
                const UInt32 next = framePixel(front, tx, ty);
                const UInt32 previous = prior ? framePixel(*prior, tx, ty) : next;
                const UInt32 expected = inside ? next : previous;
                if (next != previous) {
                    changedInside += inside;
                    changedOutside += !inside;
                }
                const UInt32 observed = row[bounds.left + x];
                if ((observed & 0xffffffU) != (expected & 0xffffffU)) {
                    if (!mismatches && attempt == 11) {
                        printf("XenosCGLWindow first front mismatch %s (%d,%d) got=%08x expected=%08x\n",
                               label, x, y, observed, expected);
                    }
                    ++mismatches;
                }
                insideCount += inside;
                outsideCount += !inside;
            }
        }
        if (!mismatches)
            break;
    }
    const int passed = !mismatches && (!prior || (changedInside && changedOutside));
    printf("XenosCGLWindow %s %s size=%dx%d front_pixels=%u clip_inside=%u clip_outside=%u changed_inside=%u changed_outside=%u mismatches=%u\n",
           passed ? "PASS" : "FAIL", label, width, height, insideCount + outsideCount,
           insideCount, outsideCount, changedInside, changedOutside, mismatches);
    fflush(0);
    return passed;
}

static int swap(AGLContext context, unsigned expectedSwaps, UInt32 ticket, int width, int height)
{
    aglSwapBuffers(context);
    unsigned aglError = aglGetError();
    int stats[4] = {0};
    glGetIntegerv(kXenosWindowStatsAttribute, stats);
    if (!checkGL("aglSwapBuffers") || aglError || stats[0] != width || stats[1] != height ||
        (unsigned)stats[2] != expectedSwaps || (UInt32)stats[3] != ticket) {
        printf("XenosCGLWindow FAIL swap agl=0x%x surface=%dx%d swaps=%d fence=%u\n",
               aglError, stats[0], stats[1], stats[2], (UInt32)stats[3]);
        return 0;
    }
    printf("XenosCGLWindow PASS swap=%u surface=%dx%d presented_fence=%u\n",
           expectedSwaps, width, height, ticket);
    fflush(0);
    return 1;
}

static int run(WindowRef window, AGLContext context)
{
    const Frame first = {37, 4, 6, 0}, second = {93, 8, 10, 1};
    const Frame resized = {121, 6, 8, 0}, clipped = {173, 8, 6, 1};
    const RectI full = {0, 0, 96, 96}, fullResized = {0, 0, 128, 96};
    const RectI clip = {24, 18, 68, 54};
    UInt32 ticket = 0;
    glViewport(0, 0, full.width, full.height);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, 1);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    if (!checkGL("setup") || !draw(first, &ticket) ||
        !swap(context, 1, ticket, full.width, full.height) ||
        !screenCheck(window, "first-front", first, 0, full) || !draw(second, &ticket) ||
        !screenCheck(window, "front-before-second-swap", first, 0, full) ||
        !swap(context, 2, ticket, full.width, full.height) ||
        !screenCheck(window, "second-front", second, 0, full))
        return 0;
    SizeWindow(window, fullResized.width, fullResized.height, 1);
    RunCurrentEventLoop(0.2);
    if (!aglUpdateContext(context)) {
        printf("XenosCGLWindow FAIL aglUpdateContext=0x%x\n", aglGetError());
        return 0;
    }
    glViewport(0, 0, fullResized.width, fullResized.height);
    int viewport[4] = {0};
    glGetIntegerv(GL_VIEWPORT, viewport);
    if (!checkGL("resize viewport") || viewport[2] != fullResized.width || viewport[3] != fullResized.height) {
        printf("XenosCGLWindow FAIL resized viewport=%dx%d\n", viewport[2], viewport[3]);
        return 0;
    }
    if (!draw(resized, &ticket) || !swap(context, 3, ticket, fullResized.width, fullResized.height) ||
        !screenCheck(window, "resized-front", resized, 0, fullResized))
        return 0;
    const int swapRectangle[] = {clip.x, clip.y, clip.width, clip.height};
    if (!aglSetInteger(context, kAGLSwapRectangle, swapRectangle) ||
        !aglEnable(context, kAGLSwapRectangle)) {
        printf("XenosCGLWindow FAIL AGL swap clipping=0x%x\n", aglGetError());
        return 0;
    }
    return draw(clipped, &ticket) &&
           swap(context, 4, ticket, fullResized.width, fullResized.height) &&
           screenCheck(window, "clipped-front", clipped, &resized, clip);
}

int main(void)
{
    const int attributes[] = {kAGLRGBA, kDoubleBufferAttribute, kWindowAttribute, kNoRecoveryAttribute,
                              kCGLPFARendererID, kWindowRenderer, 0};
    const QDRect frame = {kSceneY, kSceneX, kSceneY + 96, kSceneX + 96};
    WindowRef window = 0;
    AGLPixelFormat format = 0;
    AGLContext context = 0;
    int renderer = 0, doubleBuffered = 0, passed = 0;
    if (!loadAPI())
        return 1;
    int status = CreateNewWindow(kPlainWindowClass, kWindowNoShadow, &frame, &window);
    if (status || !window) {
        printf("XenosCGLWindow FAIL CreateNewWindow=%d\n", status);
        return 1;
    }
    ShowWindow(window);
    RunCurrentEventLoop(0.15);
    format = aglChoosePixelFormat(0, 0, attributes);
    if (!format || !aglDescribePixelFormat(format, kCGLPFARendererID, &renderer) ||
        renderer != kWindowRenderer ||
        !aglDescribePixelFormat(format, kDoubleBufferAttribute, &doubleBuffered) || !doubleBuffered) {
        printf("XenosCGLWindow FAIL aglChoosePixelFormat=0x%x renderer=0x%08x\n", aglGetError(), (UInt32)renderer);
        goto done;
    }
    context = aglCreateContext(format, 0);
    if (!context || !aglSetDrawable(context, GetWindowPort(window)) || !aglSetCurrentContext(context)) {
        printf("XenosCGLWindow FAIL AGL drawable setup=0x%x\n", aglGetError());
        goto done;
    }
    const unsigned char *name = glGetString(GL_RENDERER);
    if (!name || strcmp((const char *)name, "Xenos window drawable test")) {
        printf("XenosCGLWindow FAIL GL_RENDERER=%s\n", name ? (const char *)name : "(null)");
        goto done;
    }
    printf("XenosCGLWindow GL_RENDERER=%s renderer=0x%08x\n", name, (UInt32)renderer);
    fflush(0);
    passed = run(window, context);
done:
    if (context) {
        aglSetCurrentContext(0);
        aglSetDrawable(context, 0);
        aglDestroyContext(context);
    }
    if (format)
        aglDestroyPixelFormat(format);
    DisposeWindow(window);
    if (passed)
        printf("XenosCGLWindow PASS textured quads, buffer swaps, resize and clipping\n");
    return !passed;
}
