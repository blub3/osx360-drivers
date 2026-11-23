#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "XenosQuadProtocol.h"

static kern_return_t createSurface(io_connect_t connection, unsigned width, unsigned height,
                                   unsigned *handle) {
    return IOConnectMethodScalarIScalarO(connection, kXenosSurfaceCreate, 2, 1, width, height,
                                          handle);
}

static kern_return_t clearSurface(io_connect_t connection, unsigned handle, unsigned argb,
                                  unsigned *ticket) {
    return IOConnectMethodScalarIScalarO(connection, kXenosSurfaceClear, 2, 1, handle, argb,
                                          ticket);
}

static kern_return_t readSurface(io_connect_t connection, unsigned handle, unsigned ticket,
                                 unsigned firstRow, XenosSurfaceReadback *readback) {
    const XenosSurfaceReadRequest request = {handle, ticket, firstRow};
    IOByteCount bytes = sizeof(*readback);
    const kern_return_t result = IOConnectMethodStructureIStructureO(connection, kXenosSurfaceRead,
        sizeof(request), &bytes, (void *)&request, readback);
    return result == KERN_SUCCESS && bytes != sizeof(*readback) ? kIOReturnUnderrun : result;
}

static kern_return_t releaseSurface(io_connect_t connection, unsigned handle) {
    return IOConnectMethodScalarIScalarO(connection, kXenosSurfaceRelease, 1, 0, handle);
}

static int expect(const char *operation, kern_return_t actual, kern_return_t expected) {
    if (actual == expected)
        return 1;
    fprintf(stderr, "%s returned 0x%08x, expected 0x%08x\n", operation, (unsigned)actual,
            (unsigned)expected);
    return 0;
}

static int clearAndRead(io_connect_t connection, unsigned handle, unsigned argb,
                        unsigned *ticketOut) {
    unsigned ticket = 0;
    kern_return_t result = kIOReturnBusy;
    for (unsigned attempt = 0; attempt < 200 && result == kIOReturnBusy; ++attempt) {
        result = clearSurface(connection, handle, argb, &ticket);
        if (result == kIOReturnBusy)
            usleep(10000);
    }
    if (!expect("clear surface", result, KERN_SUCCESS) || !ticket)
        return 0;

    unsigned state = kXenosQuadPending, fence = 0, checked = 0, misses = 0, driverHash = 0;
    for (unsigned attempt = 0; attempt < 200 && state == kXenosQuadPending; ++attempt) {
        result = IOConnectMethodScalarIScalarO(connection, kXenosQuadUploadStatus, 1,
            kXenosQuadUploadResultWords, ticket, &state, &fence, &checked, &misses, &driverHash);
        if (!expect("read GPU fence status", result, KERN_SUCCESS))
            return 0;
        if (state == kXenosQuadPending)
            usleep(10000);
    }
    if (state != kXenosQuadPassed || fence != ticket || checked != kXenosQuadReadbackPixels || misses) {
        fprintf(stderr, "FAIL handle=%u ticket=%u state=%u fence=%u pixels=%u mismatches=%u\n",
                handle, ticket, state, fence, checked, misses);
        return 0;
    }

    uint32_t hash = XENOS_QUAD_HASH_INITIAL;
    unsigned appMisses = 0;
    for (unsigned row = 0; row < kXenosSurfaceSide; row += kXenosSurfaceReadRows) {
        XenosSurfaceReadback readback;
        if (!expect("read surface pixels", readSurface(connection, handle, ticket, row, &readback),
                    KERN_SUCCESS))
            return 0;
        for (unsigned i = 0; i < kXenosSurfaceReadPixels; ++i) {
            const uint32_t pixel = readback.argb[i];
            if (!appMisses && pixel != argb)
                fprintf(stderr, "Readback pixel (%u,%u) is %08x, expected %08x\n",
                        i % kXenosSurfaceSide, row + i / kXenosSurfaceSide, (unsigned)pixel, argb);
            appMisses += pixel != argb;
            hash = xenosQuadHashByte(hash, (pixel >> 16) & 0xff);
            hash = xenosQuadHashByte(hash, (pixel >> 8) & 0xff);
            hash = xenosQuadHashByte(hash, pixel & 0xff);
        }
    }
    const int passed = !appMisses && hash == driverHash;
    printf("%s handle=%u ticket=%u fence=%u argb=%08x pixels=%u mismatches=%u hash=%08x\n",
           passed ? "PASS" : "FAIL", handle, ticket, fence, argb, checked, appMisses, (unsigned)hash);
    *ticketOut = ticket;
    return passed;
}

static int runLifecycle(io_connect_t connection, io_connect_t other) {
    unsigned handle = 0, otherHandle = 0, invalid = 0, firstTicket = 0, secondTicket = 0;
    unsigned newHandle = 0, newTicket = 0;
    XenosSurfaceReadback unused;
    if (!expect("reject unsupported dimensions", createSurface(connection, 31, 32, &invalid),
                kIOReturnBadArgument) ||
        !expect("create surface", createSurface(connection, kXenosSurfaceSide, kXenosSurfaceSide,
                &handle), KERN_SUCCESS) || !handle ||
        !expect("reject duplicate allocation", createSurface(connection, 32, 32, &invalid),
                kIOReturnBusy) ||
        !expect("reject read before clear", readSurface(connection, handle, 0, 0, &unused),
                kIOReturnNotReady) ||
        !expect("reject non-opaque clear", clearSurface(connection, handle, 0x00ffffff, &invalid),
                kIOReturnBadArgument))
        return 0;

    if (!expect("create second client's surface", createSurface(other, 32, 32, &otherHandle),
                KERN_SUCCESS) || !otherHandle || handle == otherHandle ||
        !expect("reject foreign clear", clearSurface(other, handle, 0xff123456, &invalid),
                kIOReturnBadArgument) ||
        !expect("reject foreign read", readSurface(other, handle, 0, 0, &unused), kIOReturnBadArgument) ||
        !expect("reject foreign release", releaseSurface(other, handle), kIOReturnBadArgument) ||
        !expect("release second client's surface", releaseSurface(other, otherHandle), KERN_SUCCESS))
        return 0;

    if (!clearAndRead(connection, handle, 0xff4cb237, &firstTicket) ||
        !clearAndRead(connection, handle, 0xffd14285, &secondTicket) ||
        firstTicket == secondTicket ||
        !expect("reject stale ticket", readSurface(connection, handle, firstTicket, 0, &unused),
                kIOReturnBadArgument) ||
        !expect("reject out-of-bounds band", readSurface(connection, handle, secondTicket, 25, &unused),
                kIOReturnBadArgument) ||
        !expect("release surface", releaseSurface(connection, handle), KERN_SUCCESS) ||
        !expect("reject second release", releaseSurface(connection, handle), kIOReturnBadArgument) ||
        !expect("reject read after release", readSurface(connection, handle, secondTicket, 0, &unused),
                kIOReturnBadArgument))
        return 0;

    if (!expect("recreate surface", createSurface(connection, 32, 32, &newHandle), KERN_SUCCESS) ||
        !newHandle || newHandle == handle ||
        !expect("reject old handle after recreation", clearSurface(connection, handle, 0xff2148ce, &invalid),
                kIOReturnBadArgument) ||
        !clearAndRead(connection, newHandle, 0xff2148ce, &newTicket) ||
        !expect("release recreated surface", releaseSurface(connection, newHandle), KERN_SUCCESS))
        return 0;

    printf("PASS lifecycle clears=3 pixels=3072 reused_handle=%u recreated_handle=%u "
           "foreign_handles_rejected=3 stale_handles_rejected=3 stale_tickets_rejected=1\n",
           handle, newHandle);
    return 1;
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        puts("XenosSurface 1");
        return 0;
    }
    if (argc != 1) {
        fprintf(stderr, "Usage: %s [--version]\n", argv[0]);
        return 2;
    }

    io_service_t service = IOServiceGetMatchingService(kIOMasterPortDefault,
                                                         IOServiceMatching("XenosController"));
    if (!service) {
        fputs("XenosController was not found. Boot the reusable surface test.\n", stderr);
        return 1;
    }
    io_connect_t connection = MACH_PORT_NULL, other = MACH_PORT_NULL;
    int passed = expect("open primary connection", IOServiceOpen(service, mach_task_self(), 0,
                        &connection), KERN_SUCCESS);
    if (passed)
        passed = expect("open second connection", IOServiceOpen(service, mach_task_self(), 0,
                        &other), KERN_SUCCESS);
    IOObjectRelease(service);
    if (passed)
        passed = runLifecycle(connection, other);
    if (other != MACH_PORT_NULL)
        IOServiceClose(other);
    if (connection != MACH_PORT_NULL)
        IOServiceClose(connection);
    return passed ? 0 : 1;
}
