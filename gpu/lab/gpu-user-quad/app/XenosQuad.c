#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "XenosQuadProtocol.h"

int main(int argc, char **argv) {
    unsigned int texture = 0;
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        puts("XenosQuad 1");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "1") == 0) {
        texture = 1;
    } else if (argc != 1 && (argc != 2 || strcmp(argv[1], "0") != 0)) {
        fprintf(stderr, "Usage: %s [0|1|--version]\n", argv[0]);
        return 2;
    }

    io_service_t service = IOServiceGetMatchingService(kIOMasterPortDefault,
                                                       IOServiceMatching("XenosController"));
    if (!service) {
        fputs("XenosController was not found. Boot the GPU user-quad test.\n", stderr);
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
    error = IOConnectMethodScalarIScalarO(connection, kXenosQuadSubmit, 1, 1, texture, &ticket);
    if (error != KERN_SUCCESS || !ticket) {
        fprintf(stderr, "Quad submission failed (0x%08x).\n", (unsigned)error);
        IOServiceClose(connection);
        return 1;
    }
    printf("Submitted ticket=%u texture=%u\n", ticket, texture);
    fflush(stdout);

    unsigned int state = kXenosQuadPending, fence = 0, pixels = 0, mismatches = 0;
    for (unsigned int attempt = 0; attempt < 200; ++attempt) {
        error = IOConnectMethodScalarIScalarO(connection, kXenosQuadStatus, 1, kXenosQuadResultWords,
                                               ticket, &state, &fence, &pixels, &mismatches);
        if (error != KERN_SUCCESS || state != kXenosQuadPending)
            break;
        usleep(10000);
    }
    IOServiceClose(connection);
    if (error != KERN_SUCCESS) {
        fprintf(stderr, "Could not read ticket %u status (0x%08x).\n", ticket, (unsigned)error);
        return 1;
    }

    const int passed = state == kXenosQuadPassed && fence == ticket && pixels == 1024 && mismatches == 0;
    printf("%s ticket=%u texture=%u state=%u fence=%u pixels=%u mismatches=%u\n",
           passed ? "PASS" : "FAIL", ticket, texture, state, fence, pixels, mismatches);
    return passed ? 0 : 1;
}
