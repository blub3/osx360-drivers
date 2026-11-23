from pathlib import Path
import re
import subprocess

root = Path(__file__).resolve().parent
directory = root / "source/XenonCoreGraphics/src"
controller = (directory / "XenosController.cpp").read_text()

def function_from(source, name):
    match = re.search(r"^(?:static )?\w+ (?:XenosController::)?" + name + r"\([^{}]*\)\s*(?:const\s*)?\{", source, re.M)
    assert match, name
    depth = 1
    end = match.end()
    while depth:
        char = source[end]
        end += 1
        depth += (char == "{") - (char == "}")
    return source[match.start():end]

constants = "\n".join(line for line in controller.splitlines() if re.match(
    r"static const (?:UInt32|IOPhysicalAddress) k(?:DisplayMemoryStart|QuadReadbackOffset|QuadFenceOffset|SurfaceStorageBytes|SurfaceAllocationLimit|ClearPixels) =", line))
header = (directory / "XenosController.hpp").read_text()
declarations = header[header.index("  static IOReturn createClientSurface"):
                      header.index("  IOReturn enqueueQuadDepthBatch")]
source = """#include "XenosQuadProtocol.h"
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
    void startQuad(XenosQuadClient *, UInt32, bool, UInt32 mode, UInt32, UInt32 *output) {
        *output = ++_quadTicket;
        _quadClient = static_cast<XenosQuadClient *>(pendingClient);
        _quadGeometryMode = mode;
    }
    void finishQuad(UInt32 state, UInt32 fence, UInt32 checked, UInt32 mismatches, UInt32 hash) {
        UInt32 result[] = {state, fence, checked, mismatches, hash};
        memcpy(_quadClient->_result, result, sizeof(result));
        _quadClient = nullptr;
    }
    XenosQuadClient *pendingClient;
""" + declarations + "};\n" + constants + "\n" + "\n".join(function_from(controller, name) for name in (
    "resolveTileWord", "quadHashColor", "quadQueueReady", "ownsSurface", "createClientSurface",
    "queueSurfaceClear", "readClientSurface", "dropSurface", "releaseClientSurface", "closeClientSurface",
    "verifySurfaceClear")) + """
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
""" + "\n".join(line for line in (root / "app/XenosSurface.c").read_text().splitlines()
                  if not line.startswith(("#include <IOKit", "#include <mach"))) + """
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
"""
source = source.replace("void startQuad(XenosQuadClient *,", "void startQuad(XenosQuadClient *client,")
source = source.replace("_quadClient = static_cast<XenosQuadClient *>(pendingClient);", "_quadClient = client;")
check = root / "check-surface.cpp"
check.write_text(source)
subprocess.run(["c++", "-std=c++11", "-Wall", "-Wextra", "-Werror", "-I", str(directory), str(check),
                "-o", str(root / "check-surface")], check=True)
subprocess.run([str(root / "check-surface")], check=True)

sources = "\n".join(path.read_text() for path in directory.glob("*.cpp"))
fixed_costs = {"writeRing": 1, "writeRingPacket0": 1, "writeRingPacket3": 1, "writeRingRegister": 2}
functions = set(re.findall(r"^\w+ XenosController::(\w+)\(", sources, re.M))

def ring_cost(name):
    if name in fixed_costs:
        return fixed_costs[name]
    body = function_from(sources, name).split("{", 1)[1]
    calls = re.findall(r"\b(\w+)\(", body)
    return sum(ring_cost(call) for call in calls if call in fixed_costs or call in functions)

count = ring_cost("submitSurfaceClear")
budget = int(re.search(r"kQuadRingDwords = (\d+)", controller)[1])
assert count <= budget
print(f"Surface clear packet count: {count} of {budget} ring dwords.")
