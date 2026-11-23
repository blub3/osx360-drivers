#include <IOKit/IOKitLib.h>
#include <errno.h>
#include <mach/mach.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "XenosQuadProtocol.h"

static void generateRGB(uint8_t *rgb, uint32_t seed) {
    for (unsigned i = 0; i < kXenosQuadUploadBytes; ++i) {
        seed = seed * 1664525U + 1013904223U;
        rgb[i] = (uint8_t)(seed >> 24);
    }
}

static int parseSeed(const char *arg, uint32_t *seed) {
    if (arg[0] < '0' || arg[0] > '9')
        return 0;
    errno = 0;
    char *end = NULL;
    const unsigned long value = strtoul(arg, &end, 10);
    if (errno || *end || value != (uint32_t)value)
        return 0;
    *seed = (uint32_t)value;
    return 1;
}

static int parseScissor(char *const *args, XenosQuadScissor *rect) {
    uint8_t bounds[4];
    for (unsigned i = 0; i < 4; ++i) {
        uint32_t edge;
        if (!parseSeed(args[i], &edge) || edge > kXenosQuadTargetSide)
            return 0;
        bounds[i] = (uint8_t)edge;
    }
    const XenosQuadScissor parsed = {bounds[0], bounds[1], bounds[2], bounds[3]};
    if (!xenosQuadValidScissor(&parsed))
        return 0;
    *rect = parsed;
    return 1;
}

enum {
    kLowAlpha = 64,
    kHighAlpha = 192,
};

static void makeBatch(XenosQuadBlendedBatchUpload *upload, uint32_t seed) {
    XenosQuadBatchLayer *back = &upload->batch.layers[0];
    XenosQuadBatchLayer *front = &upload->batch.layers[1];
    back->kind = front->kind = kXenosQuadAxisAligned;
    back->payload.geometry = xenosQuadMakeGeometry(2, 3, 18, 19, 0, 0, 16, 16);
    front->payload.geometry = xenosQuadMakeGeometry(11, 9, 29, 29, 12, 2, 3, 12);
    generateRGB(back->payload.rgb, seed);
    generateRGB(front->payload.rgb, seed ^ 0x9e3779b9U);
    for (unsigned v = 0; v < kXenosQuadUploadSide; ++v)
        for (unsigned u = 0; u < kXenosQuadUploadSide; ++u)
            upload->frontAlpha[v * kXenosQuadUploadSide + u] =
                ((u / 4) ^ (v / 4)) & 1 ? kHighAlpha : kLowAlpha;
}

// Evaluate the UV at the center of the pixel using the submitted vertices.
static int sampleIndex(const XenosQuadGeometry *geometry, int x, int y) {
    const XenosQuadVertex topLeft = geometry->vertices[2];
    const XenosQuadVertex bottomRight = geometry->vertices[1];
    if (x < topLeft.x || x >= bottomRight.x || y < topLeft.y || y >= bottomRight.y)
        return -1;

    const int width = 2 * (bottomRight.x - topLeft.x);
    const int height = 2 * (bottomRight.y - topLeft.y);
    const int offsetX = 2 * (x - topLeft.x) + 1;
    const int offsetY = 2 * (y - topLeft.y) + 1;
    const int u = (topLeft.u * (width - offsetX) + bottomRight.u * offsetX) / width;
    const int v = (topLeft.v * (height - offsetY) + bottomRight.v * offsetY) / height;
    return v * kXenosQuadUploadSide + u;
}

typedef struct {
    uint32_t hash;
    unsigned scissorRejectOnly, stencilRejectOnly, bothReject, depthReject;
    unsigned frontOnly, lowOverlap, highOverlap;
} ClipExpectation;

static int pixelInRect(const XenosQuadScissor *rect, int x, int y) {
    // Compare pixel centers with the edges in the uploaded rectangle.
    const int centerX = 2 * x + 1, centerY = 2 * y + 1;
    return 2 * rect->left < centerX && centerX < 2 * rect->right &&
           2 * rect->top < centerY && centerY < 2 * rect->bottom;
}

static int pixelInMask(const XenosQuadStencilBatchUpload *batch, int x, int y) {
    for (unsigned i = 0; i < kXenosQuadStencilRects; ++i) {
        if (pixelInRect(&batch->mask[i], x, y))
            return 1;
    }
    return 0;
}

static ClipExpectation expectedBatch(const XenosQuadDepthBatchUpload *depth) {
    const XenosQuadScissorStencilBatchUpload *batch = &depth->clip;
    const XenosQuadBlendedBatchUpload *upload = &batch->batch.upload;
    const XenosQuadGeometryUpload *back = &upload->batch.layers[0].payload;
    const XenosQuadGeometryUpload *front = &upload->batch.layers[1].payload;
    const uint8_t clearRGB[] = {0x4c, 0xb2, 0x37};
    ClipExpectation expected = {XENOS_QUAD_HASH_INITIAL, 0, 0, 0, 0, 0, 0, 0};
    const int frontZ = 2 * depth->depthSplitX + 16;
    for (int y = 0; y < kXenosQuadTargetSide; ++y) {
        for (int x = 0; x < kXenosQuadTargetSide; ++x) {
            const int backIndex = sampleIndex(&back->geometry, x, y);
            int frontIndex = sampleIndex(&front->geometry, x, y);
            if (frontIndex >= 0) {
                const int scissor = pixelInRect(&batch->frontScissor, x, y);
                const int stencil = pixelInMask(&batch->batch, x, y);
                expected.scissorRejectOnly += !scissor && stencil;
                expected.stencilRejectOnly += scissor && !stencil;
                expected.bothReject += !scissor && !stencil;
                if (!scissor || !stencil)
                    frontIndex = -1;
                else if (backIndex >= 0 && frontZ >= 2 * x + 17) {
                    // Depths measured in units of 1/128 at the pixel center.
                    ++expected.depthReject;
                    frontIndex = -1;
                }
            }
            if (backIndex < 0 && frontIndex < 0)
                continue;
            const uint8_t *dst = backIndex >= 0 ? back->rgb + 3 * backIndex : clearRGB;
            const uint8_t *src = frontIndex >= 0 ? front->rgb + 3 * frontIndex : dst;
            const unsigned alpha = frontIndex >= 0 ? upload->frontAlpha[frontIndex] : 0;
            if (backIndex < 0)
                ++expected.frontOnly;
            else if (frontIndex >= 0 && alpha == kLowAlpha)
                ++expected.lowOverlap;
            else if (frontIndex >= 0 && alpha == kHighAlpha)
                ++expected.highOverlap;
            for (unsigned channel = 0; channel < 3; ++channel) {
                // Equivalent to interpolation with integer round-to-nearest.
                const int weighted = dst[channel] * 255 + (int)alpha * (src[channel] - dst[channel]);
                expected.hash = xenosQuadHashByte(expected.hash, (uint8_t)((weighted + 127) / 255));
            }
        }
    }
    return expected;
}

int main(int argc, char **argv) {
    uint32_t seed = 1;
    uint32_t depthSplitX = 16;
    XenosQuadDepthBatchUpload upload;
    const XenosQuadScissor testMask[] = {{13, 11, 17, 17}, {16, 18, 24, 24}};
    const XenosQuadScissor testScissor = {14, 12, 21, 22};
    memcpy(upload.clip.batch.mask, testMask, sizeof(upload.clip.batch.mask));
    upload.clip.frontScissor = testScissor;
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        puts("XenosQuadBatchDepth 1");
        return 0;
    }
    for (int i = 1; i < argc;) {
        const char *flag = argv[i++];
        if (strcmp(flag, "--seed") == 0 && i < argc && parseSeed(argv[i], &seed)) {
            ++i;
        } else if (strcmp(flag, "--mask") == 0 && argc - i >= 8 &&
                   parseScissor(argv + i, &upload.clip.batch.mask[0]) &&
                   parseScissor(argv + i + 4, &upload.clip.batch.mask[1])) {
            i += 8;
        } else if (strcmp(flag, "--scissor") == 0 && argc - i >= 4 &&
                   parseScissor(argv + i, &upload.clip.frontScissor)) {
            i += 4;
        } else if (strcmp(flag, "--depth-split") == 0 && i < argc &&
                   parseSeed(argv[i], &depthSplitX) && depthSplitX <= kXenosQuadTargetSide) {
            ++i;
        } else {
            fprintf(stderr, "Usage: %s [--seed UINT32] [--depth-split X] [--scissor LEFT TOP RIGHT BOTTOM] "
                            "[--mask LEFT TOP RIGHT BOTTOM LEFT TOP RIGHT BOTTOM] [--version]\n", argv[0]);
            return 2;
        }
    }

    upload.depthSplitX = (uint8_t)depthSplitX;
    makeBatch(&upload.clip.batch.upload, seed);
    const ClipExpectation expected = expectedBatch(&upload);

    io_service_t service = IOServiceGetMatchingService(kIOMasterPortDefault,
                                                       IOServiceMatching("XenosController"));
    if (!service) {
        fputs("XenosController was not found. Boot the quad batch depth test.\n", stderr);
        return 1;
    }
    io_connect_t connection = MACH_PORT_NULL;
    kern_return_t error = IOServiceOpen(service, mach_task_self(), 0, &connection);
    IOObjectRelease(service);
    if (error != KERN_SUCCESS) {
        fprintf(stderr, "Could not open the Xenos quad test (0x%08x).\n", (unsigned)error);
        return 1;
    }

    unsigned int ticket = 0;
    IOByteCount outputSize = sizeof(ticket);
    error = IOConnectMethodStructureIStructureO(connection, kXenosQuadUploadDepthBatch,
                                                 sizeof(upload), &outputSize, &upload, &ticket);
    if (error != KERN_SUCCESS || outputSize != sizeof(ticket) || !ticket) {
        fprintf(stderr, "Depth batch upload failed (0x%08x).\n", (unsigned)error);
        IOServiceClose(connection);
        return 1;
    }
    printf("Uploaded ticket=%u bytes=%u draws=%u stencil_draws=%u seed=%u split_x=%u "
           "scissor_reject_only=%u stencil_reject_only=%u both_reject=%u depth_reject=%u front_only=%u "
           "overlap_alpha64=%u overlap_alpha192=%u reference_rgb_hash=%08x\n",
           ticket, (unsigned)sizeof(upload), (unsigned)kXenosQuadBatchLayers,
           (unsigned)kXenosQuadStencilRects, (unsigned)seed, (unsigned)depthSplitX,
           expected.scissorRejectOnly, expected.stencilRejectOnly, expected.bothReject,
           expected.depthReject, expected.frontOnly,
           expected.lowOverlap, expected.highOverlap,
           (unsigned)expected.hash);
    fflush(stdout);

    unsigned int state = kXenosQuadPending, fence = 0, pixels = 0, mismatches = 0, hash = 0;
    for (unsigned int attempt = 0; attempt < 200; ++attempt) {
        error = IOConnectMethodScalarIScalarO(connection, kXenosQuadUploadStatus, 1,
                                               kXenosQuadUploadResultWords, ticket, &state,
                                               &fence, &pixels, &mismatches, &hash);
        if (error != KERN_SUCCESS || state != kXenosQuadPending)
            break;
        usleep(10000);
    }
    IOServiceClose(connection);
    if (error != KERN_SUCCESS) {
        fprintf(stderr, "Could not read ticket %u status (0x%08x).\n", ticket, (unsigned)error);
        return 1;
    }

    // The kernel checks each pixel against the reference within the blend
    // tolerance. The raw checksum can differ by a hardware rounding step.
    const int passed = state == kXenosQuadPassed && fence == ticket &&
                       pixels == kXenosQuadReadbackPixels && mismatches == 0;
    printf("%s ticket=%u state=%u fence=%u pixels=%u mismatches=%u blend_tolerance=%u "
           "readback_rgb_hash=%08x\n", passed ? "PASS" : "FAIL", ticket, state, fence, pixels,
           mismatches, (unsigned)kXenosQuadBlendTolerance, hash);
    return passed ? 0 : 1;
}
