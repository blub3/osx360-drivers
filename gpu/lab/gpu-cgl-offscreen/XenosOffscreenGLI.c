#include "Offscreen.h"

/* 10.4.11 PowerPC GLI v2.3 renderer and pixel-format records. */
typedef struct TigerFormat {
    struct TigerFormat *next;
    UInt32 rendererID, flags, buffering, colors, accumulation, depth, stencil;
    unsigned short auxBuffers, auxDepthStencil, sampleBuffers, samples;
    UInt32 sampleMode;
    UInt32 options;
    UInt32 displayMask;
    UInt32 reserved;
} TigerFormat;
typedef char TigerFormatSize[sizeof(TigerFormat) == 56 ? 1 : -1];

enum {
    kTigerImplicitAttribute = 4,
    kGLIOffscreen = 0x4,
    kGLIColorFormat = 0x400,
    kARGB8888Format = 0x8000,
    kTigerDispatchEntries = 682
};
typedef void (*Entry)(void);
typedef struct {
    int width, height, rowBytes;
    unsigned char *base;
} OffscreenDrawable;

typedef struct {
    XenosBackend backend;
    OffscreenDrawable drawable;
    XenosQuadGeometryUpload upload;
    UInt32 argb[kXenosQuadReadbackPixels];
    XenosFence fence;
    unsigned error, texture, vertices;
    int legacy, enabled, uploaded, recording, pending, imageReady, cleared;
    float u, v;
    XenosQuadVertex corners[4];
} Context;

static unsigned unsupported(Context *context)
{
    context->error = GL_INVALID_OPERATION;
    return 0;
}

static const unsigned char *getString(Context *context, unsigned selector)
{
    if (selector == GL_RENDERER)
        return (const unsigned char *)"Xenos offscreen test";
    unsupported(context);
    return 0;
}

static unsigned getError(Context *context)
{
    const unsigned error = context->error;
    context->error = 0;
    return error;
}

static void getIntegerGL(Context *context, unsigned selector, int *value)
{
    if (selector == kXenosFenceAttribute) {
        value[0] = (int)context->fence.ticket;
        value[1] = (int)context->fence.fence;
        value[2] = (int)context->fence.pixels;
        value[3] = (int)context->fence.hash;
    } else {
        *value = 0;
        context->error = GL_INVALID_ENUM;
    }
}

static void setViewport(Context *context, int x, int y, int width, int height)
{
    if (x || y || width != kXenosQuadTargetSide || height != kXenosQuadTargetSide)
        unsupported(context);
}

static void enable(Context *context, unsigned capability)
{
    if (capability != GL_TEXTURE_2D || context->recording)
        unsupported(context);
    else
        context->enabled = 1;
}

static void bindTexture(Context *context, unsigned target, unsigned texture)
{
    if (target != GL_TEXTURE_2D || texture != 1 || context->recording || context->pending)
        unsupported(context);
    else
        context->texture = texture;
}

static void textureParameter(Context *context, unsigned target, unsigned selector, int value)
{
    if (target != GL_TEXTURE_2D || !context->texture || value != GL_NEAREST ||
        (selector != GL_TEXTURE_MIN_FILTER && selector != GL_TEXTURE_MAG_FILTER))
        unsupported(context);
}

static void textureImage(Context *context, unsigned target, int level, int internalFormat,
                         int width, int height, int border, unsigned format, unsigned type,
                         const void *data)
{
    if (!data || target != GL_TEXTURE_2D || !context->texture || level ||
        width != kXenosQuadUploadSide || height != kXenosQuadUploadSide || border ||
        internalFormat != GL_RGB || format != GL_RGB || type != GL_UNSIGNED_BYTE ||
        context->recording || context->pending) {
        unsupported(context);
        return;
    }
    const unsigned char *pixels = data;
    const unsigned rowBytes = kXenosQuadUploadSide * 3;
    for (int y = 0; y < kXenosQuadUploadSide; ++y)
        memcpy(context->upload.rgb + y * rowBytes,
               pixels + (kXenosQuadUploadSide - y - 1) * rowBytes, rowBytes);
    context->uploaded = 1;
}

static unsigned channel(float value)
{
    return value >= 0.0f && value <= 1.0f ? (unsigned)(value * 255.0f + 0.5f) : 256U;
}

static void clearColor(Context *context, float r, float g, float b, float a)
{
    if (channel(r) != 0x4c || channel(g) != 0xb2 || channel(b) != 0x37 || channel(a) != 0xff)
        unsupported(context);
}

static void clear(Context *context, unsigned mask)
{
    if (mask != GL_COLOR_BUFFER_BIT || !context->drawable.base ||
        context->recording || context->pending) {
        unsupported(context);
        return;
    }
    context->cleared = 1;
    context->imageReady = 0;
    context->fence = (XenosFence){.ticket = 0};
}

static void begin(Context *context, unsigned mode)
{
    if (mode != GL_QUADS || context->recording || context->pending ||
        !context->enabled || !context->uploaded || !context->cleared) {
        unsupported(context);
        return;
    }
    context->vertices = 0;
    context->recording = 1;
}

static void texCoord(Context *context, float u, float v)
{
    context->u = u;
    context->v = v;
}

static int toGrid(float value, unsigned side, uint8_t *output)
{
    if (!(value >= 0.0f && value <= 1.0f))
        return 0;
    const float position = value * side;
    const unsigned index = (unsigned)position;
    if ((float)index != position)
        return 0;
    *output = (uint8_t)index;
    return 1;
}

static void vertex(Context *context, float x, float y)
{
    if (!context->recording || context->vertices >= 4) {
        unsupported(context);
        return;
    }
    XenosQuadVertex *v = &context->corners[context->vertices];
    if (!toGrid((x + 1.0f) * 0.5f, kXenosQuadTargetSide, &v->x) ||
        !toGrid((1.0f - y) * 0.5f, kXenosQuadTargetSide, &v->y) ||
        !toGrid(context->u, kXenosQuadUploadSide, &v->u) ||
        !toGrid(1.0f - context->v, kXenosQuadUploadSide, &v->v)) {
        unsupported(context);
        return;
    }
    ++context->vertices;
}

static void end(Context *context)
{
    if (!context->recording) {
        unsupported(context);
        return;
    }
    context->recording = 0;
    /* GL_QUADS perimeter order to the Xenos triangle-strip order. */
    XenosQuadGeometry geometry = {{context->corners[0], context->corners[1],
                                   context->corners[3], context->corners[2]}};
    XenosQuadMapping mapping;
    if (context->vertices != 4 || context->error || !xenosQuadDecodeGeometry(&geometry, &mapping)) {
        unsupported(context);
        return;
    }
    context->upload.geometry = geometry;
    context->pending = 1;
    context->cleared = 0;
}

static void writeRGBA(unsigned char *output, UInt32 pixel)
{
    output[0] = (unsigned char)(pixel >> 16);
    output[1] = (unsigned char)(pixel >> 8);
    output[2] = (unsigned char)pixel;
    output[3] = (unsigned char)(pixel >> 24);
}

static void finish(Context *context)
{
    if (context->recording) {
        unsupported(context);
        return;
    }
    if (!context->pending)
        return;
    context->pending = 0;
    if (!XenosRenderQuad(&context->backend, &context->upload, context->legacy,
                         &context->fence, context->argb)) {
        unsupported(context);
        return;
    }
    if (context->legacy)
        return;
    context->imageReady = 1;
    /* Keep CGL's client buffer in the same bottom-up RGBA layout as glReadPixels. */
    for (int y = 0; y < kXenosQuadTargetSide; ++y) {
        const UInt32 *row = context->argb + (kXenosQuadTargetSide - y - 1) * kXenosQuadTargetSide;
        unsigned char *out = context->drawable.base + y * context->drawable.rowBytes;
        for (int x = 0; x < kXenosQuadTargetSide; ++x)
            writeRGBA(out + 4 * x, row[x]);
    }
}

static void readPixels(Context *context, int x, int y, int width, int height,
                       unsigned format, unsigned type, void *output)
{
    finish(context);
    if (!output || !context->imageReady || format != GL_RGBA || type != GL_UNSIGNED_BYTE ||
        x < 0 || y < 0 || width <= 0 || height <= 0 || x >= kXenosQuadTargetSide ||
        y >= kXenosQuadTargetSide || width > kXenosQuadTargetSide - x ||
        height > kXenosQuadTargetSide - y) {
        unsupported(context);
        return;
    }
    unsigned char *out = output;
    for (int dy = 0; dy < height; ++dy) {
        const UInt32 *row = context->argb +
            (kXenosQuadTargetSide - y - dy - 1) * kXenosQuadTargetSide + x;
        for (int dx = 0; dx < width; ++dx)
            writeRGBA(out + 4 * (dy * width + dx), row[dx]);
    }
}

void gliInitializeLibrary(void) {}
void gliTerminateLibrary(void) {}

int gliGetVersion(int *major, int *minor, int *plugin)
{
    *major = 2;
    *minor = 3;
    *plugin = kOffscreenPlugin;
    return 1;
}

int gliChoosePixelFormat(TigerFormat **out, const UInt32 *attributes)
{
    *out = 0;
    int offscreen = 0;
    for (unsigned i = 0; i < 16 && attributes[i]; ++i) {
        switch (attributes[i]) {
        case kOffscreenAttribute:
            offscreen = 1;
            break;
        case kCGLPFARendererID:
            if (attributes[++i] != kOffscreenRenderer)
                return 0;
            break;
        case kColorSizeAttribute:
            if (attributes[++i] != 32)
                return 0;
            break;
        case kNoRecoveryAttribute:
        case kTigerImplicitAttribute:
            break;
        default:
            return 0;
        }
    }
    XenosDisplay display;
    if (!offscreen || !XenosFindDisplay(&display))
        return 0;
    TigerFormat *format = calloc(1, sizeof(*format));
    if (!format)
        return kCGLBadAlloc;
    *format = (TigerFormat){.rendererID = kOffscreenRenderer,
                            .flags = kGLIOffscreen | kGLIColorFormat,
                            .colors = kARGB8888Format,
                            .displayMask = display.mask};
    *out = format;
    return 0;
}

void gliDestroyPixelFormat(TigerFormat *format) { free(format); }

int gliQueryRendererInfo(TigerFormat **out, UInt32 displayMask)
{
    *out = 0;
    XenosDisplay display;
    if (!XenosFindDisplay(&display) || display.mask != displayMask)
        return 0;
    TigerFormat *record = calloc(1, sizeof(*record));
    if (!record)
        return kCGLBadAlloc;
    /* Query records use offset 0x2c for their display mask. */
    *record = (TigerFormat){.rendererID = kOffscreenRenderer,
                            .flags = kGLIOffscreen | kGLIColorFormat,
                            .buffering = 5, .colors = kARGB8888Format,
                            .options = display.mask};
    *out = record;
    return 0;
}

void gliDestroyRendererInfo(TigerFormat *record) { free(record); }

int gliCreateContext(Context **result, const TigerFormat *format, Context *shared,
                     Entry *parentDispatch, Entry *dispatch, unsigned flags)
{
    (void)parentDispatch;
    (void)flags;
    *result = 0;
    if (shared || format->rendererID != kOffscreenRenderer)
        return kCGLBadMatch;
    Context *context = calloc(1, sizeof(*context));
    if (!context)
        return kCGLBadAlloc;
    const char *legacy = getenv("XENOS_CGL_LEGACY_FENCE_ONLY");
    context->legacy = legacy && !strcmp(legacy, "1");
    if (!XenosOpenBackend(&context->backend)) {
        free(context);
        return kCGLBadMatch;
    }
    for (unsigned i = 0; i < kTigerDispatchEntries; ++i)
        dispatch[i] = (Entry)unsupported;
    /* The subcontext's dispatch starts at 0xc. Slot zero mirrors offset
       0x4 of the public CGL context, which also has a leading data pointer. */
#define GL_ENTRY(offset, fn) dispatch[((offset) - 4) / 4] = (Entry)(fn)
    GL_ENTRY(0x14, begin);
    GL_ENTRY(0x18, bindTexture);
    GL_ENTRY(0x2c, clear);
    GL_ENTRY(0x34, clearColor);
    GL_ENTRY(0x124, enable);
    GL_ENTRY(0x12c, end);
    GL_ENTRY(0x168, finish);
    GL_ENTRY(0x16c, finish);
    GL_ENTRY(0x19c, getError);
    GL_ENTRY(0x1a4, getIntegerGL);
    GL_ENTRY(0x1d8, getString);
    GL_ENTRY(0x3b8, readPixels);
    GL_ENTRY(0x3f0, setViewport);
    GL_ENTRY(0x430, texCoord);
    GL_ENTRY(0x4b8, textureImage);
    GL_ENTRY(0x4c4, textureParameter);
    GL_ENTRY(0x4e4, vertex);
    GL_ENTRY(0x540, setViewport);
#undef GL_ENTRY
    *result = context;
    return 0;
}

int gliDestroyContext(Context *context)
{
    XenosCloseBackend(&context->backend);
    free(context);
    return 0;
}

int gliAttachDrawableWithOptions(Context *context, unsigned type, const OffscreenDrawable *drawable,
                                  int screen, unsigned options)
{
    (void)options;
    if (!type) {
        context->drawable = (OffscreenDrawable){.base = 0};
        context->pending = context->imageReady = 0;
        return 0;
    }
    if (type != kOffscreenDrawable || screen != 0 || !drawable || !drawable->base ||
        drawable->width != kXenosQuadTargetSide || drawable->height != kXenosQuadTargetSide ||
        drawable->rowBytes != kXenosQuadTargetSide * 4)
        return kCGLBadOffScreen;
    context->drawable = *drawable;
    return 0;
}

int gliAttachDrawable(Context *context, unsigned type, const OffscreenDrawable *drawable,
                      int screen)
{
    return gliAttachDrawableWithOptions(context, type, drawable, screen, 0);
}

int gliGetInteger(Context *context, unsigned selector, int *value)
{
    (void)context;
    (void)selector;
    *value = 0;
    return kCGLBadMatch;
}

#define GLI_UNUSED(name) int name(void) { return kCGLBadMatch; }
GLI_UNUSED(gliSetInteger)
GLI_UNUSED(gliSwapBuffers)
GLI_UNUSED(gliGetAttribute)
GLI_UNUSED(gliSetAttribute)
GLI_UNUSED(gliCopyAttributes)
