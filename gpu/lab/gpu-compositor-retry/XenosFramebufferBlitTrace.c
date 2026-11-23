#include "TigerAPI.h"
typedef const void *CFRef;

int main(void)
{
    void *cg = dlopen("/System/Library/Frameworks/ApplicationServices.framework/Versions/A/"
        "Frameworks/CoreGraphics.framework/Versions/A/CoreGraphics", kDlopenNow);
    void *io = dlopen("/System/Library/Frameworks/IOKit.framework/Versions/A/IOKit", kDlopenNow);
    void *cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation", kDlopenNow);
    if (!cg || !io || !cf)
        return 1;
    CGDirectDisplayID (*mainDisplay)(void) = dlsym(cg, "CGMainDisplayID");
    io_service_t (*service)(CGDirectDisplayID) = dlsym(cg, "CGDisplayIOServicePort");
    int (*conforms)(io_object_t, const char *) = dlsym(io, "IOObjectConformsTo");
    int (*findAccelerator)(io_service_t, io_service_t *, UInt32 *) = dlsym(io, "IOAccelFindAccelerator");
    int (*blitEngine)(io_service_t, void **, int *) = dlsym(io, "IOPSAllocateBlitEngine");
    void (*deallocate)(void *) = dlsym(io, "IOPSBlitDeallocate");
    CFRef (*property)(io_service_t, CFRef, CFRef, UInt32) = dlsym(io, "IORegistryEntryCreateCFProperty");
    CFRef (*string)(CFRef, const char *, UInt32) = dlsym(cf, "CFStringCreateWithCString");
    void (*release)(CFRef) = dlsym(cf, "CFRelease");
    int (*releaseIO)(io_object_t) = dlsym(io, "IOObjectRelease");
    if (!mainDisplay || !service || !conforms || !findAccelerator || !blitEngine ||
        !deallocate || !property || !string || !release || !releaseIO)
        return 1;
    io_service_t fb = service(mainDisplay()), accelerator = 0;
    UInt32 index = 0;
    if (!fb || !conforms(fb, "XenonFramebufferTest") ||
        findAccelerator(fb, &accelerator, &index) || !accelerator || index != 0)
        return 1;
    CFRef key = string(0, "IOAccelRevision", 0x600);
    if (!key)
        return 1;
    CFRef revision = property(fb, key, 0, 0);
    int haveRevision = revision != 0;
    if (revision)
        release(revision);
    release(key);
    void *engine = 0;
    int score = 0;
    const int status = blitEngine(fb, &engine, &score);
    printf("XenosFramebufferBlitTrace accelerator_index=%u IOAccelRevision=%s "
           "IOPSAllocateBlitEngine=0x%08x engine=%p score=%d\n", index,
           haveRevision ? "present" : "absent", (UInt32)status, engine, score);
    if (!status && engine)
        deallocate(engine);
    releaseIO(accelerator);
    return 0;
}
