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

source = """#include "XenosQuadProtocol.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
using UInt32 = uint32_t;
using IOOptionBits = unsigned;
using IOReturn = int;
enum {
    kIOReturnSuccess, kIOReturnBadArgument, kIOReturnBusy, kIOReturnNoResources,
    kIOReturnNotReady, kIOReturnUnsupported, kIOReturnNotPermitted, kIOMapReadOnly = 0x1000,
    kSurfaceAllocationLimit = 8
};
struct OSObject {
    bool stopped = false;
    bool isInactive() const { return stopped; }
};
struct IOMemoryDescriptor {
    unsigned references = 1;
    void retain() { ++references; }
    void release() { assert(references); --references; }
};
using IOBufferMemoryDescriptor = IOMemoryDescriptor;
struct XenosQuadClient : OSObject {
    IOBufferMemoryDescriptor *_surface, *_surfacePixels, *_drawableView;
    UInt32 _surfaceHandle, _surfaceTicket;
    bool _drawableReader, _surfaceReady;
};
struct XenosController : OSObject {
    IOBufferMemoryDescriptor *_drawableGrantPixels;
    XenosDrawableToken _drawableGrantToken;
    UInt32 _surfaceCount, _drawableReaders;
    bool ownsSurface(XenosQuadClient *, UInt32) const;
    void dropSurface(XenosQuadClient *);
    void revokeDrawableGrant();
    static IOReturn grantDrawable(OSObject *, void *, void *, void *, void *);
    static IOReturn openDrawable(OSObject *, void *, void *, void *, void *);
    static IOReturn getDrawableView(OSObject *, void *, void *, void *, void *);
    static IOReturn closeClientSurface(OSObject *, void *, void *, void *, void *);
};
static void read_random(void *output, unsigned bytes) {
    static UInt32 sample = 0x12345678;
    assert(bytes == sizeof(XenosDrawableToken));
    auto *token = static_cast<XenosDrawableToken *>(output);
    for (unsigned i = 0; i < kXenosDrawableTokenWords; ++i)
        token->words[i] = ++sample;
}
static void *scalar(UInt32 value) {
    return reinterpret_cast<void *>(static_cast<uintptr_t>(value));
}
""" + "\n".join(function_from(controller, name) for name in ("ownsSurface", "dropSurface",
    "revokeDrawableGrant", "grantDrawable", "openDrawable", "getDrawableView", "closeClientSurface")) + """
int main() {
    XenosController device = {};
    XenosQuadClient owner = {}, first = {}, second = {};
    first._drawableReader = second._drawableReader = true;
    IOBufferMemoryDescriptor storage, pixels;
    owner._surface = &storage;
    owner._surfacePixels = &pixels;
    owner._surfaceHandle = 17;
    device._surfaceCount = 1;

    XenosDrawableToken token = {}, wrong = {};
    IOMemoryDescriptor *mapped = nullptr;
    IOOptionBits flags = 0;
    assert(device.grantDrawable(&device, &owner, scalar(17), &token, nullptr) == kIOReturnNotReady);
    owner._surfaceReady = true;
    assert(device.grantDrawable(&device, &owner, scalar(1), &token, nullptr) == kIOReturnBadArgument);
    assert(device.grantDrawable(&device, &owner, scalar(17), &token, nullptr) == kIOReturnSuccess);
    assert(device.grantDrawable(&device, &owner, scalar(17), &wrong, nullptr) == kIOReturnBusy);
    assert(device.getDrawableView(&device, &first, &flags, &mapped, nullptr) == kIOReturnNotReady);
    assert(device.openDrawable(&device, &first, &wrong, nullptr, nullptr) == kIOReturnNotPermitted);
    assert(device.openDrawable(&device, &owner, &token, nullptr, nullptr) == kIOReturnUnsupported);
    assert(device.openDrawable(&device, &first, &token, nullptr, nullptr) == kIOReturnSuccess);
    assert(device.openDrawable(&device, &second, &token, nullptr, nullptr) == kIOReturnNotPermitted);
    assert(device.getDrawableView(&device, &first, &flags, &mapped, nullptr) == kIOReturnSuccess);
    assert(flags == kIOMapReadOnly && mapped == &pixels && pixels.references == 3);
    assert(storage.references == 1);

    assert(device.grantDrawable(&device, &owner, scalar(17), &token, nullptr) == kIOReturnSuccess);
    assert(device.closeClientSurface(&device, &owner, nullptr, nullptr, nullptr) == kIOReturnSuccess);
    assert(storage.references == 0 && pixels.references == 2 && device._surfaceCount == 0);
    assert(device.openDrawable(&device, &second, &token, nullptr, nullptr) == kIOReturnNotPermitted);
    assert(device.closeClientSurface(&device, &first, nullptr, nullptr, nullptr) == kIOReturnSuccess);
    assert(pixels.references == 1 && device._drawableReaders == 0);
    mapped->release();
    assert(pixels.references == 0);
    puts("Drawable model passed owner checks, one-use grant, read-only mapping, revocation, and backing lifetime.");
}
"""
check = root / "check-drawable.cpp"
check.write_text(source)
subprocess.run(["c++", "-std=c++11", "-Wall", "-Wextra", "-Werror", "-I", str(directory), str(check),
                "-o", str(root / "check-drawable")], check=True)
subprocess.run([str(root / "check-drawable")], check=True)
