#include "TigerAPI.h"

typedef struct {
    int (*IORegistryEntryGetParentEntry)(io_object_t, const char *, io_object_t *);
    int (*IORegistryEntryGetChildIterator)(io_object_t, const char *, io_iterator_t *);
    io_object_t (*IOIteratorNext)(io_iterator_t);
    int (*IOObjectRelease)(io_object_t);
    int (*IOObjectConformsTo)(io_object_t, const char *);
} IOKitAPI;

/* Require the Xbox framebuffer and GPU controller on the same PCI device. */
static int isXenosFramebuffer(const IOKitAPI *io, io_service_t service)
{
    io_service_t node = service;
    while (node) {
        io_service_t parent = 0;
        if (io->IOObjectConformsTo(node, "XenonFramebufferTest")) {
            io_iterator_t children = 0;
            int found = 0;
            if (!io->IORegistryEntryGetParentEntry(node, "IOService", &parent)) {
                if (io->IOObjectConformsTo(parent, "IOPCIDevice") &&
                    !io->IORegistryEntryGetChildIterator(parent, "IOService", &children)) {
                    io_object_t child;
                    while ((child = io->IOIteratorNext(children)) != 0) {
                        found |= io->IOObjectConformsTo(child, "XenosController") != 0;
                        io->IOObjectRelease(child);
                    }
                    io->IOObjectRelease(children);
                }
                io->IOObjectRelease(parent);
            }
            if (node != service)
                io->IOObjectRelease(node);
            return found;
        }
        io->IORegistryEntryGetParentEntry(node, "IOService", &parent);
        if (node != service)
            io->IOObjectRelease(node);
        node = parent;
    }
    return 0;
}

int XenosFindDisplay(XenosDisplay *result)
{
    static const char coreGraphics[] = "/System/Library/Frameworks/ApplicationServices.framework/"
        "Versions/A/Frameworks/CoreGraphics.framework/Versions/A/CoreGraphics";
    static const char ioKit[] = "/System/Library/Frameworks/IOKit.framework/Versions/A/IOKit";
    void *ioLibrary = dlopen(ioKit, kDlopenNow);
    if (!ioLibrary)
        return 0;
    void *library = dlopen(coreGraphics, kDlopenNow);
    if (!library) {
        dlclose(ioLibrary);
        return 0;
    }
#define IOKIT_SYMBOL(name) .name = dlsym(ioLibrary, #name)
    const IOKitAPI io = {
        IOKIT_SYMBOL(IORegistryEntryGetParentEntry),
        IOKIT_SYMBOL(IORegistryEntryGetChildIterator),
        IOKIT_SYMBOL(IOIteratorNext),
        IOKIT_SYMBOL(IOObjectRelease),
        IOKIT_SYMBOL(IOObjectConformsTo),
    };
#undef IOKIT_SYMBOL
    int (*activeDisplays)(UInt32, CGDirectDisplayID *, UInt32 *) =
        dlsym(library, "CGGetActiveDisplayList");
    UInt32 (*displayMask)(CGDirectDisplayID) =
        dlsym(library, "CGDisplayIDToOpenGLDisplayMask");
    io_service_t (*displayService)(CGDirectDisplayID) =
        dlsym(library, "CGDisplayIOServicePort");
    unsigned long (*displayWidth)(CGDirectDisplayID) =
        dlsym(library, "CGDisplayPixelsWide");
    unsigned long (*displayHeight)(CGDirectDisplayID) =
        dlsym(library, "CGDisplayPixelsHigh");
    CGDirectDisplayID displays[32];
    UInt32 count = 0;
    int found = 0;

    if (activeDisplays && displayMask && displayService && displayWidth &&
        displayHeight && io.IORegistryEntryGetParentEntry &&
        io.IORegistryEntryGetChildIterator && io.IOIteratorNext &&
        io.IOObjectRelease && io.IOObjectConformsTo && !activeDisplays(32, displays, &count)) {
        for (UInt32 i = 0; i < count; ++i) {
            UInt32 mask = displayMask(displays[i]);
            if (mask && isXenosFramebuffer(&io, displayService(displays[i]))) {
                result->display = displays[i];
                result->mask = mask;
                result->width = displayWidth(displays[i]);
                result->height = displayHeight(displays[i]);
                found = 1;
                break;
            }
        }
    }
    dlclose(library);
    dlclose(ioLibrary);
    return found;
}
