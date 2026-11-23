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

static int readRGB(const char *path, uint8_t *rgb) {
    FILE *input = fopen(path, "rb");
    if (!input) {
        perror(path);
        return 0;
    }
    const size_t bytes = fread(rgb, 1, kXenosQuadUploadBytes, input);
    const int valid = bytes == kXenosQuadUploadBytes && fgetc(input) == EOF && !ferror(input);
    fclose(input);
    if (!valid)
        fprintf(stderr, "%s must contain exactly %u bytes of row-major 16x16 RGB data.\n",
                path, (unsigned)kXenosQuadUploadBytes);
    return valid;
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

static int parseEdges(char *const args[], uint8_t *edges, uint32_t limit) {
    for (unsigned i = 0; i < 4; ++i) {
        uint32_t value;
        if (!parseSeed(args[i], &value) || value > limit)
            return 0;
        edges[i] = (uint8_t)value;
    }
    return 1;
}

// Model sampling at the pixel center using exact rational arithmetic.
static unsigned sample(unsigned first, unsigned last, unsigned offset, unsigned extent) {
    const int start = (int)first, end = (int)last;
    const int denominator = 2 * (int)extent;
    return (unsigned)((start * denominator + (2 * (int)offset + 1) * (end - start)) / denominator);
}

static uint32_t expectedHash(const XenosQuadGeometryUpload *upload, const XenosQuadMapping *mapping) {
    uint32_t hash = XENOS_QUAD_HASH_INITIAL;
    const unsigned width = mapping->x1 - mapping->x0, height = mapping->y1 - mapping->y0;
    for (unsigned y = 0; y < height; ++y) {
        for (unsigned x = 0; x < width; ++x) {
            const unsigned u = sample(mapping->u0, mapping->u1, x, width);
            const unsigned v = sample(mapping->v0, mapping->v1, y, height);
            const uint8_t *rgb = upload->rgb + 3 * (v * kXenosQuadUploadSide + u);
            for (unsigned channel = 0; channel < 3; ++channel)
                hash = xenosQuadHashByte(hash, rgb[channel]);
        }
    }
    return hash;
}

int main(int argc, char **argv) {
    XenosQuadGeometryUpload upload;
    uint8_t rect[] = {5, 7, 29, 19};
    uint8_t uv[] = {14, 2, 2, 14};
    uint32_t seed = 1;
    const char *file = NULL;
    int hasSeed = 0;
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        puts("XenosQuadGeometry 1");
        return 0;
    }
    for (int i = 1; i < argc;) {
        const char *flag = argv[i++];
        if (strcmp(flag, "--rect") == 0 && argc - i >= 4 &&
            parseEdges(argv + i, rect, kXenosQuadTargetSide)) {
            i += 4;
        } else if (strcmp(flag, "--uv") == 0 && argc - i >= 4 &&
                   parseEdges(argv + i, uv, kXenosQuadUploadSide)) {
            i += 4;
        } else if (strcmp(flag, "--seed") == 0 && i < argc && !file && !hasSeed &&
                   parseSeed(argv[i], &seed)) {
            ++i;
            hasSeed = 1;
        } else if (strcmp(flag, "--rgb") == 0 && i < argc && !hasSeed && !file) {
            file = argv[i++];
        } else {
            fprintf(stderr, "Usage: %s [--rect X0 Y0 X1 Y1] [--uv U0 V0 U1 V1] "
                            "[--seed UINT32 | --rgb FILE] [--version]\n", argv[0]);
            return 2;
        }
    }

    upload.geometry = xenosQuadMakeGeometry(rect[0], rect[1], rect[2], rect[3],
                                            uv[0], uv[1], uv[2], uv[3]);
    XenosQuadMapping mapping;
    if (!xenosQuadDecodeGeometry(&upload.geometry, &mapping)) {
        fputs("Invalid quad: use a positive rectangle and 1x, 2x, or 4x texel scale on each axis.\n", stderr);
        return 2;
    }
    if (file) {
        if (!readRGB(file, upload.rgb))
            return 2;
    } else {
        generateRGB(upload.rgb, seed);
    }
    const uint32_t expected = expectedHash(&upload, &mapping);

    io_service_t service = IOServiceGetMatchingService(kIOMasterPortDefault,
                                                       IOServiceMatching("XenosController"));
    if (!service) {
        fputs("XenosController was not found. Boot the quad geometry test.\n", stderr);
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
    error = IOConnectMethodStructureIStructureO(connection, kXenosQuadUploadGeometry,
                                                 sizeof(upload), &outputSize, &upload, &ticket);
    if (error != KERN_SUCCESS || outputSize != sizeof(ticket) || !ticket) {
        fprintf(stderr, "Geometry upload failed (0x%08x).\n", (unsigned)error);
        IOServiceClose(connection);
        return 1;
    }
    printf("Uploaded ticket=%u bytes=%u rect=%u,%u,%u,%u uv=%u,%u,%u,%u expected_hash=%08x\n",
           ticket, (unsigned)sizeof(upload), (unsigned)rect[0], (unsigned)rect[1],
           (unsigned)rect[2], (unsigned)rect[3], (unsigned)uv[0], (unsigned)uv[1],
           (unsigned)uv[2], (unsigned)uv[3], (unsigned)expected);
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
                       pixels == kXenosQuadReadbackPixels && mismatches == 0 && hash == expected;
    printf("%s ticket=%u state=%u fence=%u pixels=%u mismatches=%u expected_hash=%08x gpu_hash=%08x\n",
           passed ? "PASS" : "FAIL", ticket, state, fence, pixels, mismatches, (unsigned)expected, hash);
    return passed ? 0 : 1;
}
