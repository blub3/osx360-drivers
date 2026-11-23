#include "XenosWindowCopy.h"

enum { side = 64, width = 18, height = 18, total = side * side };
static UInt32 pixels[total], before[total];

int main(void)
{
    for (unsigned i = 0; i < total; ++i)
        pixels[i] = before[i] = 0xff000000U | ((i * 0x1d38e3U) & 0xffffffU);
    const XenosWindowPixels screen = {pixels, side * sizeof(UInt32), side, side};
    const XenosCopyPlan plan = {1, width * height, {{{4, 4, 14, 8}, width, height}}};
    XenosFullCopy copy;
    XenosCopyPlan invalid = plan;
    invalid.area = 33 * 32;
    invalid.rects[0].width = 33;
    invalid.rects[0].height = 32;
    const char *failure = XenosCompositeUnarmedCopy(&screen, &invalid, &copy);
    if (!failure || copy.committed || copy.batches)
        return 1;
    for (unsigned i = 0; i < total; ++i)
        if (pixels[i] != before[i])
            return 1;
    failure = XenosCompositeUnarmedCopy(&screen, &plan, &copy);
    unsigned guard = 0;
    for (unsigned y = 0; y < side; ++y) {
        for (unsigned x = 0; x < side; ++x) {
            const unsigned i = y * side + x;
            const int covered = x >= 14 && x < 14 + width && y >= 8 && y < 8 + height;
            const UInt32 expected = covered ? before[(y - 4) * side + x - 10] : before[i];
            if (pixels[i] != expected)
                return 1;
            guard += !covered;
        }
    }
    printf("XenosUnarmedPrivate %s ticket=%u fence=%u gpu_pixels=%u screen_pixels=%u guard=%u gpu_ms=%u reason=%s\n",
           failure ? "FAIL" : "PASS", copy.ticket, copy.fence, copy.gpuPixels, copy.screenPixels, guard,
           copy.elapsedMs, failure ? failure : "verified");
    return failure || !copy.committed || !copy.ticket || copy.ticket != copy.fence ||
        copy.batches != 1 || copy.gpuPixels != plan.area || copy.screenPixels != plan.area;
}
