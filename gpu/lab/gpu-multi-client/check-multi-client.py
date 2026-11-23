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

start = controller.index("XenosController::SurfaceQueueRecord *XenosController::surfaceQueueRecord(")
end = controller.index("bool XenosController::submitQuad()", start)
queue = controller[start:end]
header = (directory / "XenosController.hpp").read_text()
state = header[header.index("  UInt32                     _nextQuadTicket;"):
               header.index("  UInt32                     _surfaceSerial;")]
methods = header[header.index("  static IOReturn enqueueSurfaceClear"):
                 header.index("  IOReturn enqueueQuadDepthBatch")]
source = """#include "XenosQuadProtocol.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
using UInt32 = uint32_t;
using IOReturn = int;
enum {
    kIOReturnSuccess, kIOReturnBadArgument, kIOReturnBusy, kIOReturnNotReady, kIOReturnNotFound
};
struct OSObject {
    bool inactive = false;
    unsigned refs = 1;
    bool isInactive() const { return inactive; }
    void retain() { ++refs; }
    void release() { assert(refs); --refs; }
};
struct XenosQuadClient : OSObject {
    OSObject *_surface;
    UInt32 _surfaceHandle, _surfaceTicket, _ticket;
    UInt32 _result[kXenosQuadUploadResultWords];
    bool _surfaceReady;
};
static void IOLog(const char *, ...) {}
static void *scalar(UInt32 value) {
    return reinterpret_cast<void *>(static_cast<uintptr_t>(value));
}
struct XenosController : OSObject {
    XenosQuadClient *_quadClient;
    OSObject *_quadScratch;
    UInt32 _quadTicket, _surfaceColor, _surfaceCount;
""" + state + methods + """
    bool ownsSurface(XenosQuadClient *client, UInt32 handle) const {
        return client->_surface && handle == client->_surfaceHandle;
    }
    UInt32 reserveQuadTicket();
    void startQuad(XenosQuadClient *client, UInt32, bool, UInt32 mode, UInt32,
                   UInt32 *out, UInt32 ticket) {
        assert(mode == kXenosSurfaceClear && !_quadClient && !_quadScratch);
        _quadClient = client;
        client->retain();
        *out = _quadTicket = ticket;
    }
    void finishQuad(UInt32 state, UInt32, UInt32, UInt32) {
        if (state != kXenosQuadPassed) {
            if (auto *record = surfaceQueueRecord(_surfaceActiveTicket))
                record->state = kXenosSurfaceQueueFailed;
        }
        _quadClient->release();
        _quadClient = nullptr;
    }
    UInt32 submit(XenosQuadClient &client, UInt32 expectedAhead) {
        UInt32 values[2] = {};
        assert(enqueueSurfaceClear(this, &client, scalar(client._surfaceHandle),
                                  scalar(0xff123456), values) == kIOReturnSuccess);
        assert(values[1] == expectedAhead && values[0]);
        return values[0];
    }
    void beginGpu() {
        _quadScratch = _quadClient->_surface;
        _quadScratch->retain();
        surfaceQueueRecord(_surfaceActiveTicket)->state = kXenosSurfaceQueueInFlight;
    }
    void gpuComplete() {
        finishQuad(kXenosQuadPassed, _quadTicket, 1024, 0);
        _quadScratch->release();
        _quadScratch = nullptr;
        retireSurfaceQueueEntry(_quadTicket);
    }
    void closeClient(XenosQuadClient &client) {
        closeQueuedSurface(&client);
        client._surface->release();
        client._surface = nullptr;
        client._surfaceHandle = 0;
        --_surfaceCount;
    }
};
""" + function_from(controller, "reserveQuadTicket") + "\n" + queue + """
int main() {
    XenosController device = {};
    XenosQuadClient a = {}, b = {};
    OSObject aPixels, bPixels;
    a._surface = &aPixels;
    a._surfaceHandle = 21;
    b._surface = &bPixels;
    b._surfaceHandle = 22;
    device._surfaceCount = 2;
    const UInt32 a1 = device.submit(a, 0);
    const UInt32 b1 = device.submit(b, 1);
    assert(device._surfaceQueueCount == 2 && a.refs == 2 && b.refs == 2);
    UInt32 rejected[2] = {};
    assert(device.enqueueSurfaceClear(&device, &a, scalar(21), scalar(0xff123456),
                                     rejected) == kIOReturnBusy && !rejected[0]);
    for (UInt32 ticket : {a1, b1}) {
        device.startNextSurface();
        assert(device._quadTicket == ticket);
        device.beginGpu();
        device.gpuComplete();
    }
    XenosSurfaceQueueInfo info;
    assert(device.readSurfaceQueue(&device, &a, const_cast<UInt32 *>(&a1), &info, nullptr) == kIOReturnSuccess);
    assert(info.fence == a1 && info.state == kXenosSurfaceQueueComplete && info.completion == 1);
    auto *pair = device.surfaceQueueRecord(b1);
    assert(pair->fence == b1 && pair->completion == 2 && device._surfaceCount == 2);
    const UInt32 a2 = device.submit(a, 0);
    const UInt32 b2 = device.submit(b, 1);
    device.startNextSurface();
    assert(device._quadTicket == a2);
    device.beginGpu();
    device.closeClient(b);
    auto *cancelled = device.surfaceQueueRecord(b2);
    assert(cancelled->state == kXenosSurfaceQueueCancelled &&
           !cancelled->fence && cancelled->ownerClosed && !bPixels.refs);
    assert(device._surfaceCount == 1 && device._surfaceQueueCount == 0);
    device.gpuComplete();

    OSObject reopenedPixels;
    b._surface = &reopenedPixels;
    b._surfaceHandle = 23;
    ++device._surfaceCount;
    const UInt32 b3 = device.submit(b, 0);
    device.startNextSurface();
    device.beginGpu();
    device.closeClient(b);
    auto *departed = device.surfaceQueueRecord(b3);
    assert(departed->ownerClosed && departed->state == kXenosSurfaceQueueInFlight);
    assert(reopenedPixels.refs == 1 && device._surfaceActiveTicket == b3);
    const UInt32 a3 = device.submit(a, 1);
    device.gpuComplete();
    assert(departed->fence == b3 && departed->state == kXenosSurfaceQueueComplete &&
           departed->completion == 4 && !reopenedPixels.refs && !device._surfaceActiveTicket);
    device.startNextSurface();
    assert(device._quadTicket == a3);
    device.beginGpu();
    device.gpuComplete();
    assert(device.surfaceQueueRecord(a3)->completion == 5);
    device.closeClient(a);
    assert(!device._surfaceCount && !device._surfaceQueueCount && !device._surfaceActiveTicket);
    puts("FIFO, per-client admission, pending cancellation, in-flight retention and survivor completion passed.");
}
"""
source = source.replace('#include <assert.h>', '#include <assert.h>\n#include <initializer_list>')
check = root / "check-multi-client.cpp"
check.write_text(source)
subprocess.run(["c++", "-std=c++11", "-Wall", "-Wextra", "-Werror", "-I", str(directory), str(check),
                "-o", str(root / "check-multi-client")], check=True)
subprocess.run([str(root / "check-multi-client")], check=True)
