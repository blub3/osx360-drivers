from pathlib import Path
import subprocess

root = Path(__file__).resolve().parent
app = (root / "app/XenosSharedDrawable.c").read_text()
app = "\n".join(line for line in app.splitlines() if not line.startswith(("#include <IOKit", "#include <mach")))
source = r'''
#include "XenosQuadProtocol.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
typedef unsigned io_connect_t, io_service_t;
typedef size_t IOByteCount, vm_size_t;
typedef uintptr_t vm_address_t;
typedef int kern_return_t;
enum {
    KERN_SUCCESS, MACH_PORT_NULL = 0, kIOReturnBusy, kIOReturnNotPermitted, KERN_PROTECTION_FAILURE,
    kIOMasterPortDefault, kIOMapAnywhere, VM_PROT_READ = PROT_READ, VM_PROT_WRITE = PROT_WRITE
};
static uint32_t *backing;
static int readerFd, imported;
static unsigned nextTicket;
static const XenosDrawableToken token = {{12345U,45678U,78901U,90123U}};
static unsigned mach_task_self(void) { return 1; }
static unsigned IOServiceMatching(const char *name) { (void)name; return 1; }
static unsigned IOServiceGetMatchingService(unsigned p, unsigned s) { (void)p; return s; }
static void IOObjectRelease(unsigned object) { (void)object; }
static int IOServiceOpen(unsigned service, unsigned task, unsigned type, io_connect_t *connection) {
    (void)service; (void)task;
    *connection = type + 1;
    return 0;
}
static void IOServiceClose(io_connect_t connection) { (void)connection; }
static int IOConnectMapMemory(io_connect_t connection, unsigned type, unsigned task,
                              vm_address_t *address, vm_size_t *length, unsigned options) {
    assert(connection == 2 && type == kXenosDrawableMemoryType && imported);
    (void)task; (void)options;
    *length = 4096;
    void *page = mmap(NULL, *length, PROT_READ, MAP_SHARED, readerFd, 0);
    assert(page != MAP_FAILED);
    *address = (uintptr_t)page;
    return 0;
}
static int IOConnectUnmapMemory(io_connect_t connection, unsigned type, unsigned task,
                                vm_address_t address) {
    (void)connection; (void)type; (void)task;
    return munmap((void *)address, 4096);
}
static int vm_protect(unsigned task, vm_address_t address, vm_size_t length, int maximum, int flags) {
    (void)task; (void)maximum;
    if (!mprotect((void *)address, length, flags))
        return KERN_SUCCESS;
    assert(errno == EACCES);
    return KERN_PROTECTION_FAILURE;
}
static int IOConnectMethodStructureIStructureO(io_connect_t connection, unsigned verb, size_t inSize,
        IOByteCount *outSize, void *request, void *output) {
    assert(connection == 1 && verb == kXenosSurfaceRead && inSize == sizeof(XenosSurfaceReadRequest));
    assert(*outSize == sizeof(XenosSurfaceReadback));
    XenosSurfaceReadRequest *read = request;
    XenosSurfaceReadback *out = output;
    assert(read->handle == 1 && read->ticket == nextTicket);
    for (unsigned i = 0; i < kXenosSurfaceReadPixels; ++i)
        out->argb[i] = backing[read->firstRow * kXenosSurfaceSide + i];
    return 0;
}
static uint32_t modelHash(void) {
    uint32_t hash = XENOS_QUAD_HASH_INITIAL;
    for (unsigned i = 0; i < kXenosQuadReadbackPixels; ++i) {
        hash = xenosQuadHashByte(hash, (backing[i] >> 16) & 0xff);
        hash = xenosQuadHashByte(hash, (backing[i] >> 8) & 0xff);
        hash = xenosQuadHashByte(hash, backing[i] & 0xff);
    }
    return hash;
}
static int IOConnectMethodScalarIScalarO(io_connect_t connection, unsigned verb, unsigned ins,
                                         unsigned outs, ...) {
    va_list args;
    va_start(args, outs);
    unsigned inputs[4];
    unsigned *outputs[5];
    for (unsigned i = 0; i < ins; ++i)
        inputs[i] = va_arg(args, unsigned);
    for (unsigned i = 0; i < outs; ++i)
        outputs[i] = va_arg(args, unsigned *);
    va_end(args);
    if (verb == kXenosDrawableImport) {
        assert(connection == 2 && ins == 4 && !outs);
        for (unsigned i = 0; i < 4; ++i)
            if (inputs[i] != token.words[i] || imported)
                return kIOReturnNotPermitted;
        imported = 1;
        return 0;
    }
    assert(connection == 1);
    switch (verb) {
    case kXenosSurfaceCreate:
        assert(ins == 2 && outs == 1 && inputs[0] == 32 && inputs[1] == 32);
        *outputs[0] = 1;
        break;
    case kXenosSurfaceClear:
        assert(ins == 2 && outs == 1 && inputs[0] == 1);
        *outputs[0] = ++nextTicket;
        for (unsigned i = 0; i < kXenosQuadReadbackPixels; ++i)
            backing[i] = inputs[1];
        break;
    case kXenosQuadUploadStatus:
        assert(ins == 1 && outs == 5 && inputs[0] == nextTicket);
        *outputs[0] = kXenosQuadPassed;
        *outputs[1] = nextTicket;
        *outputs[2] = kXenosQuadReadbackPixels;
        *outputs[3] = 0;
        *outputs[4] = modelHash();
        break;
    case kXenosDrawableExport:
        assert(ins == 1 && outs == 4 && inputs[0] == 1);
        for (unsigned i = 0; i < 4; ++i)
            *outputs[i] = token.words[i];
        break;
    case kXenosSurfaceRelease:
        assert(ins == 1 && outs == 0 && inputs[0] == 1);
        break;
    default:
        abort();
    }
    return 0;
}
#define main tigerDrawableMain
''' + app + r'''
#undef main
int main(void) {
    char file[] = "/tmp/xenos-drawable-ipc.XXXXXX";
    const int writerFd = mkstemp(file);
    assert(writerFd >= 0 && !ftruncate(writerFd, 4096));
    readerFd = open(file, O_RDONLY);
    assert(readerFd >= 0 && !unlink(file));
    backing = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, writerFd, 0);
    assert(backing != MAP_FAILED);
    char name[] = "XenosSharedDrawable";
    char *args[] = {name, NULL};
    const int status = tigerDrawableMain(1, args);
    assert(status == 0 && nextTicket == 2);
    munmap(backing, 4096);
    close(readerFd);
    close(writerFd);
    puts("Local two-process test passed both pixel epochs using a read-only backing map.");
}
'''
check = root / "check-reader-process.c"
check.write_text(source)
subprocess.run(["cc", "-std=c99", "-D_DARWIN_C_SOURCE", "-Wall", "-Wextra", "-Werror",
                "-I", str(root / "source/XenonCoreGraphics/src"), str(check),
                "-o", str(root / "check-reader-process")], check=True)
subprocess.run([str(root / "check-reader-process")], check=True, timeout=25)
