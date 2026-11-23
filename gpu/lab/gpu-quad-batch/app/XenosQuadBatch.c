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

static void makeBatch(XenosQuadBatchUpload *batch, const char *mode, uint32_t seed, int reverse) {
    XenosQuadBatchLayer *first = &batch->layers[reverse ? 1 : 0];
    XenosQuadBatchLayer *second = &batch->layers[reverse ? 0 : 1];
    first->kind = kXenosQuadAxisAligned;
    second->kind = kXenosQuadAxisAligned;
    first->payload.geometry = xenosQuadMakeGeometry(2, 3, 18, 19, 0, 0, 16, 16);
    second->payload.geometry = xenosQuadMakeGeometry(11, 9, 29, 29, 12, 2, 3, 12);
    if (strcmp(mode, "disjoint") == 0) {
        first->payload.geometry = xenosQuadMakeGeometry(1, 2, 13, 14, 1, 0, 13, 12);
        second->payload.geometry = xenosQuadMakeGeometry(18, 17, 30, 29, 13, 2, 1, 14);
    } else if (strcmp(mode, "rotated") == 0) {
        const XenosQuadGeometry rotated = {{
            {17, 52, 14, 13}, {65, 100, 2, 13}, {57, 12, 14, 3}, {105, 60, 2, 3},
        }};
        second->kind = kXenosQuadRotated45;
        second->payload.geometry = rotated;
    }
    generateRGB(first->payload.rgb, seed);
    generateRGB(second->payload.rgb, seed ^ 0x9e3779b9U);
}

// Invert the geometry at the pixel center independently of the driver's
// axis-aligned and 45-degree point-sample calculations.
static const uint8_t *sampleLayer(const XenosQuadBatchLayer *layer, int x, int y) {
    const XenosQuadVertex *vertices = layer->payload.geometry.vertices;
    const XenosQuadVertex tl = vertices[2], tr = vertices[3], bl = vertices[0];
    const int quarter = kXenosQuadQuarter;
    const int scale = layer->kind == kXenosQuadRotated45 ? 1 : quarter;
    const int ux = scale * (tr.x - tl.x), uy = scale * (tr.y - tl.y);
    const int vx = scale * (bl.x - tl.x), vy = scale * (bl.y - tl.y);
    const int det = ux * vy - uy * vx;
    const int dx = quarter * x + quarter / 2 - scale * tl.x;
    const int dy = quarter * y + quarter / 2 - scale * tl.y;
    const int a = dx * vy - dy * vx, b = ux * dy - uy * dx;
    if (a < 0 || b < 0 || a >= det || b >= det)
        return NULL;
    const unsigned u = (tl.u * det + a * (tr.u - tl.u)) / det;
    const unsigned v = (tl.v * det + b * (bl.v - tl.v)) / det;
    return layer->payload.rgb + 3 * (v * kXenosQuadUploadSide + u);
}

typedef struct {
    uint32_t hash;
    unsigned firstOnly, secondOnly, overlap;
} BatchExpectation;

static BatchExpectation expectedBatch(const XenosQuadBatchUpload *batch) {
    BatchExpectation expected = {XENOS_QUAD_HASH_INITIAL, 0, 0, 0};
    for (int y = 0; y < kXenosQuadTargetSide; ++y) {
        for (int x = 0; x < kXenosQuadTargetSide; ++x) {
            const uint8_t *first = sampleLayer(&batch->layers[0], x, y);
            const uint8_t *second = sampleLayer(&batch->layers[1], x, y);
            if (first && second)
                ++expected.overlap;
            else if (first)
                ++expected.firstOnly;
            else if (second)
                ++expected.secondOnly;
            const uint8_t *visible = second ? second : first;
            if (visible) {
                for (unsigned channel = 0; channel < 3; ++channel)
                    expected.hash = xenosQuadHashByte(expected.hash, visible[channel]);
            }
        }
    }
    return expected;
}

int main(int argc, char **argv) {
    const char *mode = "overlap";
    uint32_t seed = 1;
    int reverse = 0;
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        puts("XenosQuadBatch 1");
        return 0;
    }
    for (int i = 1; i < argc;) {
        const char *flag = argv[i++];
        if (strcmp(flag, "--mode") == 0 && i < argc &&
            (strcmp(argv[i], "overlap") == 0 || strcmp(argv[i], "disjoint") == 0 ||
             strcmp(argv[i], "rotated") == 0)) {
            mode = argv[i++];
        } else if (strcmp(flag, "--reverse") == 0) {
            reverse = 1;
        } else if (strcmp(flag, "--seed") == 0 && i < argc && parseSeed(argv[i], &seed)) {
            ++i;
        } else {
            fprintf(stderr, "Usage: %s [--mode overlap|disjoint|rotated] [--reverse] "
                            "[--seed UINT32] [--version]\n", argv[0]);
            return 2;
        }
    }

    XenosQuadBatchUpload upload;
    makeBatch(&upload, mode, seed, reverse);
    const BatchExpectation expected = expectedBatch(&upload);

    io_service_t service = IOServiceGetMatchingService(kIOMasterPortDefault,
                                                       IOServiceMatching("XenosController"));
    if (!service) {
        fputs("XenosController was not found. Boot the two-quad batch test.\n", stderr);
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
    error = IOConnectMethodStructureIStructureO(connection, kXenosQuadUploadBatch,
                                                 sizeof(upload), &outputSize, &upload, &ticket);
    if (error != KERN_SUCCESS || outputSize != sizeof(ticket) || !ticket) {
        fprintf(stderr, "Batch upload failed (0x%08x).\n", (unsigned)error);
        IOServiceClose(connection);
        return 1;
    }
    printf("Uploaded ticket=%u bytes=%u draws=%u mode=%s reversed=%d seed=%u "
           "first_only=%u second_only=%u overlap=%u expected_hash=%08x\n",
           ticket, (unsigned)sizeof(upload), (unsigned)kXenosQuadBatchLayers, mode, reverse,
           (unsigned)seed, expected.firstOnly, expected.secondOnly, expected.overlap,
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

    const int passed = state == kXenosQuadPassed && fence == ticket &&
                       pixels == kXenosQuadReadbackPixels && mismatches == 0 && hash == expected.hash;
    printf("%s ticket=%u state=%u fence=%u pixels=%u mismatches=%u expected_hash=%08x gpu_hash=%08x\n",
           passed ? "PASS" : "FAIL", ticket, state, fence, pixels, mismatches, (unsigned)expected.hash, hash);
    return passed ? 0 : 1;
}
