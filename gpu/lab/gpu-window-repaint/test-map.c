#include "TigerAPI.h"
extern UInt32 mach_task_self_;
extern int vm_region(UInt32, UInt32 *, UInt32 *, int, int *, UInt32 *, UInt32 *);
extern int mach_port_deallocate(UInt32, UInt32);
int main(void)
{
    UInt32 connect = 0, address = 0, size = 0;
    void *io = dlopen("/System/Library/Frameworks/IOKit.framework/Versions/A/IOKit", kDlopenNow);
    void *(*match)(const char *) = dlsym(io, "IOServiceMatching");
    UInt32 (*find)(UInt32, void *) = dlsym(io, "IOServiceGetMatchingService");
    int (*open)(UInt32, UInt32, UInt32, UInt32 *) = dlsym(io, "IOServiceOpen");
    int (*map)(UInt32, UInt32, UInt32, UInt32 *, UInt32 *, UInt32) = dlsym(io, "IOConnectMapMemory");
    int (*unmap)(UInt32, UInt32, UInt32, UInt32) = dlsym(io, "IOConnectUnmapMemory");
    int (*close)(UInt32) = dlsym(io, "IOServiceClose");
    int (*release)(UInt32) = dlsym(io, "IOObjectRelease");
    UInt32 fb = find(0, match("XenonFramebufferTest"));
    int st = !fb ? -1 : open(fb, mach_task_self_, 1, &connect);
    printf("shared open=%08x connect=%u\n", (UInt32)st, connect);
    if (!st && connect) {
        st = map(connect, 110, mach_task_self_, &address, &size, 1);
        printf("VRAM map=%08x addr=%08x size=%08x\n", (UInt32)st, address, size);
        if (!st && address && size) {
            UInt32 a = address, span = 0, count = 8, object = 0;
            int info[8] = {0};
            int rv = vm_region(mach_task_self_, &a, &span, 10, info, &count, &object);
            printf("region=%08x addr=%08x size=%08x prot=%u max=%u count=%u\n",
                   (UInt32)rv, a, span, info[0], info[1], count);
            if (object) mach_port_deallocate(mach_task_self_, object);
            unmap(connect, 110, mach_task_self_, address);
        }
        close(connect);
    }
    if (fb) release(fb);
    return 0;
}
