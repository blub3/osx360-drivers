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

int main(int argc, char **argv) {
    uint8_t rgb[kXenosQuadUploadBytes];
    uint32_t seed = 1;
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        puts("XenosQuadUpload 1");
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "--rgb") == 0) {
        if (!readRGB(argv[2], rgb))
            return 2;
    } else if (argc == 1 || (argc == 3 && strcmp(argv[1], "--seed") == 0 &&
                             parseSeed(argv[2], &seed))) {
        generateRGB(rgb, seed);
    } else {
        fprintf(stderr, "Usage: %s [--seed UINT32 | --rgb FILE | --version]\n", argv[0]);
        return 2;
    }

    uint32_t expected = XENOS_QUAD_HASH_INITIAL;
    for (unsigned i = 0; i < kXenosQuadUploadBytes; ++i)
        expected = xenosQuadHashByte(expected, rgb[i]);

    io_service_t service = IOServiceGetMatchingService(kIOMasterPortDefault,
                                                       IOServiceMatching("XenosController"));
    if (!service) {
        fputs("XenosController was not found. Boot the quad upload test.\n", stderr);
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
    error = IOConnectMethodStructureIStructureO(connection, kXenosQuadUpload,
                                                 sizeof(rgb), &outputSize, rgb, &ticket);
    if (error != KERN_SUCCESS || outputSize != sizeof(ticket) || !ticket) {
        fprintf(stderr, "Texture upload failed (0x%08x).\n", (unsigned)error);
        IOServiceClose(connection);
        return 1;
    }
    printf("Uploaded ticket=%u bytes=%u input_hash=%08x\n",
           ticket, (unsigned)sizeof(rgb), (unsigned)expected);
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
    printf("%s ticket=%u state=%u fence=%u pixels=%u mismatches=%u input_hash=%08x gpu_hash=%08x\n",
           passed ? "PASS" : "FAIL", ticket, state, fence, pixels, mismatches, (unsigned)expected, hash);
    return passed ? 0 : 1;
}
