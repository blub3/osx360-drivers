#include "WindowGL.h"

int XenosLoadPresentation(XenosPresentation *api)
{
    api->library = dlopen("/System/Library/Frameworks/ApplicationServices.framework/Versions/A/"
                          "Frameworks/CoreGraphics.framework/Versions/A/CoreGraphics", kDlopenNow);
    if (!api->library)
        return 0;
#define LOAD(member, symbol) do { \
    api->member = dlsym(api->library, #symbol); \
    if (!api->member) return 0; \
} while (0)
    LOAD(getSurfaceBounds, CGSGetSurfaceBounds);
    LOAD(createWindowContext, CGWindowContextCreate);
    LOAD(releaseContext, CGContextRelease);
    LOAD(clipToRect, CGContextClipToRect);
    LOAD(setInterpolationQuality, CGContextSetInterpolationQuality);
    LOAD(drawImage, CGContextDrawImage);
    LOAD(flushContext, CGContextFlush);
    LOAD(createProvider, CGDataProviderCreateWithData);
    LOAD(releaseProvider, CGDataProviderRelease);
    LOAD(createRGB, CGColorSpaceCreateDeviceRGB);
    LOAD(releaseColorSpace, CGColorSpaceRelease);
    LOAD(createImage, CGImageCreate);
    LOAD(releaseImage, CGImageRelease);
#undef LOAD
    return 1;
}

int XenosWindowBounds(const XenosPresentation *api, const WindowSurface *surface, RectI *out)
{
    CGRect32 bounds = {0, 0, 0, 0};
    if (!surface->connection || !surface->window || !surface->surface ||
        api->getSurfaceBounds(surface->connection, surface->window, surface->surface, &bounds) ||
        !(bounds.width >= 1 && bounds.width <= 4096 && bounds.height >= 1 && bounds.height <= 4096) ||
        !(bounds.x >= -4096 && bounds.x <= 4096 && bounds.y >= -4096 && bounds.y <= 4096))
        return 0;
    *out = (RectI){(int)bounds.x, (int)bounds.y, (int)bounds.width, (int)bounds.height};
    return 1;
}

int XenosPresentWindow(const XenosPresentation *api, const WindowSurface *surface, RectI bounds,
                        RectI clip, const UInt32 *data)
{
    enum { kTileSide = kXenosQuadTargetSide, kRGBNoAlphaFirst = 6 };
    void *context = api->createWindowContext(surface->connection, surface->window, 0);
    if (!context)
        return 0;
    void *provider = api->createProvider(0, data, sizeof(*data) * kTileSide * kTileSide, 0);
    void *color = api->createRGB();
    void *image = provider && color ?
        api->createImage(kTileSide, kTileSide, 8, 32, kTileSide * 4, color,
                         kRGBNoAlphaFirst, provider, 0, 0, 0) : 0;
    if (image) {
        api->setInterpolationQuality(context, 1); /* kCGInterpolationNone */
        api->clipToRect(context, (CGRect32){bounds.x + clip.x, bounds.y + clip.y,
                                          clip.width, clip.height});
        api->drawImage(context, (CGRect32){bounds.x, bounds.y, bounds.width, bounds.height}, image);
        api->flushContext(context);
        api->releaseImage(image);
    }
    if (color)
        api->releaseColorSpace(color);
    if (provider)
        api->releaseProvider(provider);
    api->releaseContext(context);
    return image != 0;
}
