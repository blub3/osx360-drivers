#include "XenosQuadProtocol.h"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
using UInt32 = uint32_t;
using UInt8 = uint8_t;
using IOPhysicalAddress = uint32_t;
using IOByteCount = size_t;
using IOReturn = int;
enum {
    kIOReturnSuccess, kIOReturnBadArgument, kIOReturnBusy, kIOReturnNoResources,
    kIOReturnNoMemory, kIOReturnNotReady, kIOReturnUnderrun,
    KERN_SUCCESS = kIOReturnSuccess, MACH_PORT_NULL = 0, kIOMemoryPhysicallyContiguous = 0
};
#define PAGE_SIZE 4096
#define CACHELINE_SIZE 128
#define kXenosFramebufferLength 0x00800000
static void IOLog(const char *, ...) {}
static void flushMarkerCacheLine(volatile UInt32 *) {}
struct OSObject {
    bool isInactive() const { return false; }
};
struct IOBufferMemoryDescriptor {
    alignas(UInt32) UInt8 memory[3 * PAGE_SIZE];
    unsigned references = 1;
    static IOBufferMemoryDescriptor *withOptions(int, size_t size, size_t align) {
        assert(size == 3 * PAGE_SIZE && align == PAGE_SIZE);
        return new IOBufferMemoryDescriptor;
    }
    IOPhysicalAddress getPhysicalSegment(int offset, IOByteCount *length) {
        assert(offset == 0);
        *length = sizeof(memory);
        return 0x10000;
    }
    void *getBytesNoCopy() { return memory; }
    void retain() { ++references; }
    void release() { if (!--references) delete this; }
};
struct XenosQuadClient : OSObject {
    IOBufferMemoryDescriptor *_surface;
    UInt32 _surfaceHandle, _surfaceTicket;
    bool _surfaceReady;
    UInt32 _result[kXenosQuadUploadResultWords];
};
struct XenosController : OSObject {
    XenosQuadClient *_quadClient;
    IOBufferMemoryDescriptor *_quadScratch;
    UInt32 _surfaceSerial, _surfaceCount, _surfaceColor, _quadTicket, _quadGeometryMode, _quadCooldown;
    IOReturn quadQueueReady(XenosQuadClient *);
    void startQuad(XenosQuadClient *client, UInt32, bool, UInt32 mode, UInt32, UInt32 *output) {
        *output = ++_quadTicket;
        _quadClient = client;
        _quadGeometryMode = mode;
    }
    void finishQuad(UInt32 state, UInt32 fence, UInt32 checked, UInt32 mismatches, UInt32 hash) {
        UInt32 result[] = {state, fence, checked, mismatches, hash};
        memcpy(_quadClient->_result, result, sizeof(result));
        _quadClient = nullptr;
    }
    XenosQuadClient *pendingClient;
  static IOReturn createClientSurface(OSObject *owner, void *client, void *width, void *height, void *handle);
  static IOReturn queueSurfaceClear(OSObject *owner, void *client, void *handle, void *argb, void *ticket);
  static IOReturn readClientSurface(OSObject *owner, void *client, void *input, void *output, void *);
  static IOReturn releaseClientSurface(OSObject *owner, void *client, void *handle, void *, void *);
  static IOReturn closeClientSurface(OSObject *owner, void *client, void *, void *, void *);
  void dropSurface(XenosQuadClient *client);
  bool ownsSurface(XenosQuadClient *client, UInt32 handle) const;
  bool submitSurfaceClear();
  void verifySurfaceClear(UInt8 *memory, UInt32 fence);
  void restoreQuadDisplayAndFence(UInt32 fence);
};
static const IOPhysicalAddress kDisplayMemoryStart = 0x20000000 - 2 * kXenosFramebufferLength;
static const UInt32 kQuadReadbackOffset = PAGE_SIZE;
static const UInt32 kQuadFenceOffset = 2 * PAGE_SIZE;
static const UInt32 kSurfaceStorageBytes = kQuadFenceOffset + PAGE_SIZE;
static const UInt32 kSurfaceAllocationLimit = 8;
static const UInt32 kClearPixels = kXenosQuadReadbackPixels;
static UInt32 resolveTileWord(UInt32 x, UInt32 y) {
  const UInt32 pipe = (x >> 3) ^ ((y & 8) >> 2);
  const UInt32 rows = (y & 30) << 5;
  const UInt32 withinPipe = ((x & 4) << 1) | ((y & 1) << 2) | (x & 3);
  return rows | (pipe << 4) | withinPipe;
}
static UInt32 quadHashColor(UInt32 hash, UInt32 color) {
  hash = xenosQuadHashByte(hash, (color >> 16) & 0xff);
  hash = xenosQuadHashByte(hash, (color >> 8) & 0xff);
  return xenosQuadHashByte(hash, color & 0xff);
}
IOReturn XenosController::quadQueueReady(XenosQuadClient *client) {
  if (isInactive() || client->isInactive())
    return kIOReturnNotReady;
  return _quadClient || _quadScratch || _quadCooldown ? kIOReturnBusy : kIOReturnSuccess;
}
bool XenosController::ownsSurface(XenosQuadClient *client, UInt32 handle) const {
  return handle && client->_surface && handle == client->_surfaceHandle;
}
IOReturn XenosController::createClientSurface(OSObject *owner, void *clientArg, void *widthArg,
                                               void *heightArg, void *handleOut) {
  XenosController *self = static_cast<XenosController *>(owner);
  XenosQuadClient *client = static_cast<XenosQuadClient *>(clientArg);
  if (self->isInactive() || client->isInactive())
    return kIOReturnNotReady;
  if (reinterpret_cast<uintptr_t>(widthArg) != kXenosSurfaceSide ||
      reinterpret_cast<uintptr_t>(heightArg) != kXenosSurfaceSide)
    return kIOReturnBadArgument;
  if (client->_surface)
    return kIOReturnBusy;
  if (self->_surfaceCount >= kSurfaceAllocationLimit)
    return kIOReturnNoResources;

  IOBufferMemoryDescriptor *surface = IOBufferMemoryDescriptor::withOptions(
      kIOMemoryPhysicallyContiguous, kSurfaceStorageBytes, PAGE_SIZE);
  if (!surface)
    return kIOReturnNoMemory;
  IOByteCount length = 0;
  const IOPhysicalAddress physical = surface->getPhysicalSegment(0, &length);
  if (!surface->getBytesNoCopy() || !physical || length < kSurfaceStorageBytes ||
      (physical & (PAGE_SIZE - 1)) || physical > kDisplayMemoryStart - kSurfaceStorageBytes) {
    surface->release();
    return kIOReturnNoResources;
  }

  if (++self->_surfaceSerial == 0)
    ++self->_surfaceSerial;
  ++self->_surfaceCount;
  client->_surface = surface;
  client->_surfaceHandle = self->_surfaceSerial;
  client->_surfaceTicket = 0;
  client->_surfaceReady = false;
  *static_cast<UInt32 *>(handleOut) = client->_surfaceHandle;
  return kIOReturnSuccess;
}
IOReturn XenosController::queueSurfaceClear(OSObject *owner, void *clientArg, void *handleArg,
                                             void *colorArg, void *ticketOut) {
  XenosController *self = static_cast<XenosController *>(owner);
  XenosQuadClient *client = static_cast<XenosQuadClient *>(clientArg);
  const UInt32 handle = static_cast<UInt32>(reinterpret_cast<uintptr_t>(handleArg));
  const UInt32 argb = static_cast<UInt32>(reinterpret_cast<uintptr_t>(colorArg));
  if (!self->ownsSurface(client, handle) || (argb >> 24) != 0xff)
    return kIOReturnBadArgument;
  const IOReturn ready = self->quadQueueReady(client);
  if (ready != kIOReturnSuccess)
    return ready;

  self->_surfaceColor = argb;
  self->startQuad(client, 0, true, kXenosSurfaceClear, 0, static_cast<UInt32 *>(ticketOut));
  client->_surfaceReady = false;
  client->_surfaceTicket = self->_quadTicket;
  return kIOReturnSuccess;
}
IOReturn XenosController::readClientSurface(OSObject *owner, void *clientArg, void *input,
                                             void *output, void *) {
  XenosController *self = static_cast<XenosController *>(owner);
  XenosQuadClient *client = static_cast<XenosQuadClient *>(clientArg);
  const XenosSurfaceReadRequest &request = *static_cast<const XenosSurfaceReadRequest *>(input);
  if (self->isInactive() || client->isInactive())
    return kIOReturnNotReady;
  if (!self->ownsSurface(client, request.handle) ||
      request.firstRow > kXenosSurfaceSide - kXenosSurfaceReadRows)
    return kIOReturnBadArgument;
  if (request.ticket != client->_surfaceTicket)
    return kIOReturnBadArgument;
  if (!client->_surfaceReady)
    return kIOReturnNotReady;

  const UInt8 *memory = static_cast<const UInt8 *>(client->_surface->getBytesNoCopy());
  const volatile UInt32 *tiled = reinterpret_cast<const volatile UInt32 *>(memory + kQuadReadbackOffset);
  XenosSurfaceReadback &readback = *static_cast<XenosSurfaceReadback *>(output);
  for (UInt32 row = 0; row < kXenosSurfaceReadRows; ++row)
    for (UInt32 x = 0; x < kXenosSurfaceSide; ++x)
      readback.argb[row * kXenosSurfaceSide + x] = tiled[resolveTileWord(x, request.firstRow + row)];
  return kIOReturnSuccess;
}
void XenosController::dropSurface(XenosQuadClient *client) {
  if (client->_surface) {
    client->_surface->release();
    client->_surface = NULL;
    --_surfaceCount;
  }
  client->_surfaceHandle = client->_surfaceTicket = 0;
  client->_surfaceReady = false;
}
IOReturn XenosController::releaseClientSurface(OSObject *owner, void *clientArg, void *handleArg,
                                                 void *, void *) {
  XenosController *self = static_cast<XenosController *>(owner);
  XenosQuadClient *client = static_cast<XenosQuadClient *>(clientArg);
  if (!self->ownsSurface(client, static_cast<UInt32>(reinterpret_cast<uintptr_t>(handleArg))))
    return kIOReturnBadArgument;
  if (self->_quadClient == client && self->_quadGeometryMode == kXenosSurfaceClear)
    return kIOReturnBusy;
  self->dropSurface(client);
  return kIOReturnSuccess;
}
IOReturn XenosController::closeClientSurface(OSObject *owner, void *clientArg, void *, void *, void *) {
  static_cast<XenosController *>(owner)->dropSurface(static_cast<XenosQuadClient *>(clientArg));
  return kIOReturnSuccess;
}
void XenosController::verifySurfaceClear(UInt8 *memory, UInt32 fence) {
  volatile UInt32 *pixels = reinterpret_cast<volatile UInt32 *>(memory + kQuadReadbackOffset);
  for (UInt32 offset = 0; offset < PAGE_SIZE; offset += CACHELINE_SIZE)
    flushMarkerCacheLine(pixels + offset / sizeof(UInt32));

  UInt32 mismatches = 0, hash = XENOS_QUAD_HASH_INITIAL;
  for (UInt32 y = 0; y < kXenosSurfaceSide; ++y) {
    for (UInt32 x = 0; x < kXenosSurfaceSide; ++x) {
      const UInt32 observed = pixels[resolveTileWord(x, y)];
      if (!mismatches && observed != _surfaceColor)
        IOLog("XenosSurface: mismatch=(%lu,%lu) expected=%08lx observed=%08lx\n",
              x, y, _surfaceColor, observed);
      mismatches += observed != _surfaceColor;
      hash = quadHashColor(hash, observed);
    }
  }
  if (_quadClient->_surface == _quadScratch)
    _quadClient->_surfaceReady = true;
  finishQuad(mismatches ? kXenosQuadPixelsFailed : kXenosQuadPassed, fence, kClearPixels, mismatches, hash);
}
using io_connect_t = unsigned;
using io_service_t = unsigned;
using kern_return_t = int;
enum { kIOMasterPortDefault, TestMaxClients = kSurfaceAllocationLimit + 1 };
static XenosController device = {};
static XenosQuadClient clients[TestMaxClients];
static unsigned nextConnection = 0;
static bool badGpuPixel = false;
static void *scalar(unsigned word) { return reinterpret_cast<void *>(static_cast<uintptr_t>(word)); }
static unsigned mach_task_self() { return 1; }
static unsigned IOServiceMatching(const char *) { return 1; }
static unsigned IOServiceGetMatchingService(unsigned, unsigned) { return 1; }
static unsigned IOObjectRelease(unsigned) { return 0; }
static kern_return_t IOServiceOpen(unsigned, unsigned, unsigned, io_connect_t *connection) {
    *connection = ++nextConnection;
    assert(*connection <= TestMaxClients);
    return KERN_SUCCESS;
}
static kern_return_t IOServiceClose(io_connect_t connection) {
    return device.closeClientSurface(&device, &clients[connection - 1], nullptr, nullptr, nullptr);
}
static kern_return_t IOConnectMethodStructureIStructureO(io_connect_t connection, unsigned selector,
        size_t inputSize, IOByteCount *outputSize, void *input, void *output) {
    assert(selector == kXenosSurfaceRead);
    assert(inputSize == sizeof(XenosSurfaceReadRequest) && *outputSize == sizeof(XenosSurfaceReadback));
    return device.readClientSurface(&device, &clients[connection - 1], input, output, nullptr);
}
static kern_return_t IOConnectMethodScalarIScalarO(io_connect_t connection, unsigned selector,
        unsigned inputs, unsigned outputs, ...) {
    XenosQuadClient &owner = clients[connection - 1];
    va_list args;
    va_start(args, outputs);
    const unsigned first = va_arg(args, unsigned);
    int result = KERN_SUCCESS;
    if (selector == kXenosSurfaceCreate || selector == kXenosSurfaceClear) {
        assert(inputs == 2 && outputs == 1);
        const unsigned second = va_arg(args, unsigned);
        unsigned *output = va_arg(args, unsigned *);
        device.pendingClient = &owner;
        result = selector == kXenosSurfaceCreate
            ? device.createClientSurface(&device, &owner, scalar(first), scalar(second), output)
            : device.queueSurfaceClear(&device, &owner, scalar(first), scalar(second), output);
    } else if (selector == kXenosSurfaceRelease) {
        assert(inputs == 1 && outputs == 0);
        result = device.releaseClientSurface(&device, &owner, scalar(first), nullptr, nullptr);
    } else {
        assert(selector == kXenosQuadUploadStatus && inputs == 1 && outputs == 5);
        assert(device._quadClient == &owner && owner._surfaceTicket == first);
        // Substitute the pixels a fence would make visible after a hardware clear.
        device._quadScratch = owner._surface;
        auto *memory = static_cast<UInt8 *>(owner._surface->getBytesNoCopy());
        auto *pixels = reinterpret_cast<UInt32 *>(memory + kQuadReadbackOffset);
        for (unsigned i = 0; i < kClearPixels; ++i)
            pixels[i] = device._surfaceColor;
        if (badGpuPixel)
            pixels[resolveTileWord(0, 0)] ^= 1;
        device.verifySurfaceClear(memory, first);
        device._quadScratch = nullptr;
        for (unsigned index = 0; index < outputs; ++index)
            *va_arg(args, unsigned *) = owner._result[index];
    }
    va_end(args);
    return result;
}
#define main tigerSurfaceMain
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
#undef main
int main() {
    char name[] = "XenosSurface";
    char *argv[] = {name, nullptr};
    assert(tigerSurfaceMain(1, argv) == 0);
    assert(device._surfaceCount == 0);
    badGpuPixel = true;
    assert(tigerSurfaceMain(1, argv) == 1);
    assert(device._surfaceCount == 0);

    for (unsigned i = 0; i < TestMaxClients; ++i) {
        unsigned handle = 0;
        assert(createSurface(i + 1, 32, 32, &handle) ==
               (i < kSurfaceAllocationLimit ? KERN_SUCCESS : kIOReturnNoResources));
    }
    XenosQuadClient &owner = clients[0];
    auto *memory = static_cast<UInt8 *>(owner._surface->getBytesNoCopy());
    auto *tiled = reinterpret_cast<UInt32 *>(memory + kQuadReadbackOffset);
    owner._surfaceTicket = 123;
    owner._surfaceReady = true;
    for (unsigned y = 0; y < kXenosSurfaceSide; ++y)
        for (unsigned x = 0; x < kXenosSurfaceSide; ++x)
            tiled[resolveTileWord(x, y)] = y * kXenosSurfaceSide + x;
    XenosSurfaceReadback readback;
    assert(readSurface(1, owner._surfaceHandle, 123, 24, &readback) == KERN_SUCCESS);
    for (unsigned i = 0; i < kXenosSurfaceReadPixels; ++i)
        assert(readback.argb[i] == 24 * kXenosSurfaceSide + i);

    // A pending GPU job holds its own descriptor reference across client close.
    auto *inFlight = owner._surface;
    inFlight->retain();
    assert(IOServiceClose(1) == KERN_SUCCESS && !owner._surface);
    assert(inFlight->references == 1 && device._surfaceCount == kSurfaceAllocationLimit - 1);
    inFlight->release();
    for (unsigned i = 2; i <= TestMaxClients; ++i)
        assert(IOServiceClose(i) == KERN_SUCCESS);
    assert(device._surfaceCount == 0);
    puts("Surface model passed pixel ordering, lifecycle, ownership, cap, and close checks.");
}
