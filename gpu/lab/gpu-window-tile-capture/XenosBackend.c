#include "XenosWindowCopy.h"

int XenosOpenBackend(XenosBackend *backend)
{
    backend->library = dlopen("/System/Library/Frameworks/IOKit.framework/Versions/A/IOKit",
                              kDlopenNow);
    if (!backend->library)
        return 0;
    void *(*matching)(const char *) = dlsym(backend->library, "IOServiceMatching");
    UInt32 (*find)(UInt32, void *) = dlsym(backend->library, "IOServiceGetMatchingService");
    int (*open)(UInt32, UInt32, unsigned, UInt32 *) = dlsym(backend->library, "IOServiceOpen");
    int (*release)(UInt32) = dlsym(backend->library, "IOObjectRelease");
    const UInt32 *task = dlsym((void *)-2, "mach_task_self_");
#define LOAD(name) backend->name = dlsym(backend->library, #name)
    LOAD(IOServiceClose);
    LOAD(IOConnectMethodStructureIStructureO);
    LOAD(IOConnectMethodScalarIScalarO);
#undef LOAD
    if (matching && find && open && release && task && backend->IOServiceClose &&
        backend->IOConnectMethodStructureIStructureO && backend->IOConnectMethodScalarIScalarO) {
        const UInt32 service = find(0, matching("XenosController"));
        if (service) {
            int result = open(service, *task, 0, &backend->connection);
            release(service);
            if (!result && backend->connection)
                return 1;
        }
    }
    XenosCloseBackend(backend);
    return 0;
}

void XenosCloseBackend(XenosBackend *backend)
{
    if (backend->connection)
        backend->IOServiceClose(backend->connection);
    if (backend->library)
        dlclose(backend->library);
    *backend = (XenosBackend){.connection = 0};
}

int XenosCaptureTile(XenosBackend *backend, const XenosQuadTileUpload *upload,
                     XenosFence *fence, UInt32 *argb)
{
    const unsigned submit = kXenosQuadCaptureTile;
    *fence = (XenosFence){.ticket = 0};
    const UInt32 busy = 0xe00002d5U;
    unsigned long ticketBytes = sizeof(fence->ticket);
    int result = busy;
    for (unsigned attempt = 0; attempt < 48; ++attempt) {
        ticketBytes = sizeof(fence->ticket);
        result = backend->IOConnectMethodStructureIStructureO(backend->connection, submit,
                                      sizeof(*upload), &ticketBytes, upload, &fence->ticket);
        if ((UInt32)result != busy)
            break;
        usleep(5000);
    }
    if (result || !fence->ticket || ticketBytes != sizeof(fence->ticket)) {
        printf("Xenos tile capture submit selector=%u status=0x%08x\n", submit, (UInt32)result);
        return 0;
    }

    UInt32 mismatches = 0, state = kXenosQuadPending;
    for (unsigned attempt = 0; attempt < 60 && state == kXenosQuadPending; ++attempt) {
        result = backend->IOConnectMethodScalarIScalarO(backend->connection, kXenosQuadUploadStatus,
                     1, kXenosQuadUploadResultWords, fence->ticket, &state, &fence->fence,
                     &fence->pixels, &mismatches, &fence->hash);
        if (result)
            break;
        if (state == kXenosQuadPending)
            usleep(5000);
    }
    if (result || state != kXenosQuadPassed || fence->ticket != fence->fence ||
        fence->pixels != kXenosQuadReadbackPixels || mismatches) {
        printf("Xenos tile capture completion state=%u ticket=%u fence=%u checked=%u mismatches=%u status=0x%08x\n",
               state, fence->ticket, fence->fence, fence->pixels, mismatches, (UInt32)result);
        return 0;
    }
    for (UInt32 row = 0; row < kXenosQuadTargetSide; row += kXenosSurfaceReadRows) {
        const XenosQuadReadRequest request = {fence->ticket, row};
        XenosSurfaceReadback band;
        unsigned long size = sizeof(band);
        result = backend->IOConnectMethodStructureIStructureO(backend->connection,
                         kXenosQuadReadCaptured, sizeof(request), &size, &request, &band);
        if (result || size != sizeof(band)) {
            printf("Xenos tile capture read row=%u status=0x%08x bytes=%lu\n", row, (UInt32)result, size);
            return 0;
        }
        memcpy(argb + row * kXenosQuadTargetSide, band.argb, sizeof(band.argb));
    }
    return 1;
}
