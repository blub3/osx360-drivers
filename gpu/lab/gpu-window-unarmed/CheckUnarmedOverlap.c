#include "XenosWindowCopy.h"

enum { columns = 224, rows = 128, width = 98, height = 74,
       sourceX = 14, sourceY = 14, shiftX = 28, shiftY = 12,
       targetX = sourceX + shiftX, targetY = sourceY + shiftY };
static UInt32 pixels[columns * rows], before[columns * rows];

int main(void)
{
    for (unsigned i = 0; i < columns * rows; ++i)
        pixels[i] = before[i] = i * 0x1d38e3U;
    const XenosWindowPixels screen = {pixels, columns * sizeof(UInt32), columns, rows};
    const XenosCopyPlan plan = {1, width * height,
        {{{sourceX, sourceY, targetX, targetY}, width, height}}};
    XenosFullCopy copy;
    XenosCopyPlan rejected = plan;
    rejected.rects[0].move.targetX = sourceX + width;
    if (!XenosCompositeUnarmedOverlap(&screen, &rejected, &copy) || copy.committed || copy.batches)
        return 1;
    rejected = plan;
    rejected.rects[0].width = 112;
    rejected.area = 112 * height;
    if (!XenosCompositeUnarmedOverlap(&screen, &rejected, &copy) || copy.committed || copy.batches)
        return 1;
    for (unsigned i = 0; i < columns * rows; ++i)
        if (pixels[i] != before[i])
            return 1;

    const char *failure = XenosCompositeUnarmedOverlap(&screen, &plan, &copy);
    if (failure) {
        printf("XenosOverlapPrivate FAIL reason=%s completed=%u gpu_ms=%u\n",
               failure, copy.batches, copy.elapsedMs);
        return 1;
    }
    unsigned matches = 0, guard = 0;
    for (unsigned y = 0; y < rows; ++y) {
        for (unsigned x = 0; x < columns; ++x) {
            const unsigned i = y * columns + x;
            const int covered = x >= targetX && x < targetX + width && y >= targetY && y < targetY + height;
            if (covered)
                matches += pixels[i] == before[(y - shiftY) * columns + x - shiftX];
            else
                guard += pixels[i] == before[i];
        }
    }
    int ok = copy.committed && copy.batches == kFullCopyBatchLimit && copy.firstFence &&
        copy.ticket == copy.fence && copy.fence == copy.firstFence + copy.batches - 1 &&
        copy.gpuPixels == plan.area && copy.screenPixels == plan.area && matches == plan.area &&
        guard == columns * rows - plan.area;
    for (UInt32 i = 0; i < copy.batches; ++i)
        ok = ok && copy.fences[i] == copy.firstFence + i;
    char fences[kBatchFenceTextBytes];
    XenosFormatBatchFences(&copy, fences);
    printf("XenosOverlapPrivate %s batches=%u fences=%s gpu_pixels=%u copied=%u guard=%u gpu_ms=%u\n",
           ok ? "PASS" : "FAIL", copy.batches, fences, copy.gpuPixels, matches, guard, copy.elapsedMs);
    return !ok;
}
