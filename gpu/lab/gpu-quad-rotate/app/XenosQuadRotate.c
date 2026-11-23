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

static int makeRotation(XenosQuadGeometry *geometry, const uint8_t *shape,
                        const uint8_t *uv, int direction) {
    const int ox = 4 * shape[0] + 1, oy = 4 * shape[1];
    const int ux = 4 * shape[2], uy = direction * ux;
    const int vy = 4 * shape[3], vx = -direction * vy;
    const int xs[] = {ox + vx, ox + ux + vx, ox, ox + ux};
    const int ys[] = {oy + vy, oy + uy + vy, oy, oy + uy};
    for (unsigned i = 0; i < 4; ++i) {
        if (xs[i] < 0 || ys[i] < 0 ||
            xs[i] > 4 * kXenosQuadTargetSide || ys[i] > 4 * kXenosQuadTargetSide)
            return 0;
    }
    const XenosQuadGeometry result = {{
        {(uint8_t)xs[0], (uint8_t)ys[0], uv[0], uv[3]},
        {(uint8_t)xs[1], (uint8_t)ys[1], uv[2], uv[3]},
        {(uint8_t)xs[2], (uint8_t)ys[2], uv[0], uv[1]},
        {(uint8_t)xs[3], (uint8_t)ys[3], uv[2], uv[1]},
    }};
    *geometry = result;
    return 1;
}

// Use the parallelogram determinant to invert the uploaded vertices at
// each pixel center. This is independent of the driver's 45-degree projection.
static uint32_t expectedHash(const XenosQuadGeometryUpload *upload, unsigned *covered) {
    const XenosQuadVertex *vertices = upload->geometry.vertices;
    const XenosQuadVertex tl = vertices[2], tr = vertices[3], bl = vertices[0];
    const int ux = tr.x - tl.x, uy = tr.y - tl.y;
    const int vx = bl.x - tl.x, vy = bl.y - tl.y;
    const int det = ux * vy - uy * vx;
    uint32_t hash = XENOS_QUAD_HASH_INITIAL;
    *covered = 0;
    for (int y = 0; y < kXenosQuadTargetSide; ++y) {
        for (int x = 0; x < kXenosQuadTargetSide; ++x) {
            const int dx = 4 * x + 2 - tl.x, dy = 4 * y + 2 - tl.y;
            const int a = dx * vy - dy * vx, b = ux * dy - uy * dx;
            if (a < 0 || b < 0 || a >= det || b >= det)
                continue;
            const unsigned u = (tl.u * det + a * ((int)tr.u - tl.u)) / det;
            const unsigned v = (tl.v * det + b * ((int)bl.v - tl.v)) / det;
            const uint8_t *rgb = upload->rgb + 3 * (v * kXenosQuadUploadSide + u);
            for (unsigned channel = 0; channel < 3; ++channel)
                hash = xenosQuadHashByte(hash, rgb[channel]);
            ++*covered;
        }
    }
    return hash;
}

int main(int argc, char **argv) {
    XenosQuadGeometryUpload upload;
    uint8_t shape[] = {14, 3, 12, 10};
    uint8_t uv[] = {14, 3, 2, 13};
    uint32_t seed = 1;
    const char *file = NULL;
    int hasSeed = 0, hasShape = 0, direction = 1;
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        puts("XenosQuadRotate 1");
        return 0;
    }
    for (int i = 1; i < argc;) {
        const char *flag = argv[i++];
        if (strcmp(flag, "--shape") == 0 && argc - i >= 4 &&
            parseEdges(argv + i, shape, kXenosQuadTargetSide)) {
            i += 4;
            hasShape = 1;
        } else if (strcmp(flag, "--uv") == 0 && argc - i >= 4 &&
                   parseEdges(argv + i, uv, kXenosQuadUploadSide)) {
            i += 4;
        } else if (strcmp(flag, "--ccw") == 0) {
            direction = -1;
        } else if (strcmp(flag, "--seed") == 0 && i < argc && !file && !hasSeed &&
                   parseSeed(argv[i], &seed)) {
            ++i;
            hasSeed = 1;
        } else if (strcmp(flag, "--rgb") == 0 && i < argc && !hasSeed && !file) {
            file = argv[i++];
        } else {
            fprintf(stderr, "Usage: %s [--shape X Y U_LEG V_LEG] [--uv U0 V0 U1 V1] "
                            "[--ccw] [--seed UINT32 | --rgb FILE] [--version]\n", argv[0]);
            return 2;
        }
    }

    if (direction < 0 && !hasShape) {
        shape[0] = 3;
        shape[1] = 16;
    }

    XenosQuadRotation rotation;
    if (!makeRotation(&upload.geometry, shape, uv, direction) ||
        !xenosQuadDecodeRotation(&upload.geometry, &rotation)) {
        fputs("Invalid rotated quad: keep it in the 32x32 target and use whole-texel 1x, 2x, or 4x leg scales.\n", stderr);
        return 2;
    }
    if (file) {
        if (!readRGB(file, upload.rgb))
            return 2;
    } else {
        generateRGB(upload.rgb, seed);
    }
    unsigned covered = 0;
    const uint32_t expected = expectedHash(&upload, &covered);

    io_service_t service = IOServiceGetMatchingService(kIOMasterPortDefault,
                                                       IOServiceMatching("XenosController"));
    if (!service) {
        fputs("XenosController was not found. Boot the rotated quad test.\n", stderr);
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
    error = IOConnectMethodStructureIStructureO(connection, kXenosQuadUploadRotated,
                                                 sizeof(upload), &outputSize, &upload, &ticket);
    if (error != KERN_SUCCESS || outputSize != sizeof(ticket) || !ticket) {
        fprintf(stderr, "Rotated geometry upload failed (0x%08x).\n", (unsigned)error);
        IOServiceClose(connection);
        return 1;
    }
    printf("Uploaded ticket=%u bytes=%u angle=%d shape=%u,%u,%u,%u uv=%u,%u,%u,%u "
           "expected_covered=%u expected_hash=%08x\n",
           ticket, (unsigned)sizeof(upload), 45 * direction, (unsigned)shape[0],
           (unsigned)shape[1], (unsigned)shape[2], (unsigned)shape[3], (unsigned)uv[0],
           (unsigned)uv[1], (unsigned)uv[2], (unsigned)uv[3], covered, (unsigned)expected);
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
