
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
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "XenosQuadProtocol.h"

enum { kRounds = 2, kPixelPageBytes = kXenosQuadReadbackPixels * sizeof(uint32_t) };

typedef struct {
    uint32_t ticket;
    uint32_t color;
    uint32_t hash;
} Frame;

typedef struct {
    uint32_t pid;
    uint32_t ticket;
    uint32_t pixels;
    uint32_t mismatches;
    uint32_t hash;
} ReaderResult;

static int expect(const char *operation, kern_return_t actual, kern_return_t expected) {
    if (actual == expected)
        return 1;
    fprintf(stderr, "%s returned 0x%08x, expected 0x%08x\n", operation, (unsigned)actual,
            (unsigned)expected);
    return 0;
}

static io_connect_t openClient(unsigned type) {
    io_service_t service = IOServiceGetMatchingService(kIOMasterPortDefault,
                                                      IOServiceMatching("XenosController"));
    if (!service) {
        fputs("XenosController was not found. Boot the shared drawable test.\n", stderr);
        return MACH_PORT_NULL;
    }
    io_connect_t connection = MACH_PORT_NULL;
    const kern_return_t result = IOServiceOpen(service, mach_task_self(), type, &connection);
    IOObjectRelease(service);
    return expect("open Xenos client", result, KERN_SUCCESS) ? connection : MACH_PORT_NULL;
}

static int transfer(int fd, void *data, size_t length, int sending) {
    unsigned char *bytes = data;
    while (length) {
        const ssize_t amount = sending ? write(fd, bytes, length) : read(fd, bytes, length);
        if (amount <= 0) {
            if (amount < 0 && errno == EINTR)
                continue;
            return 0;
        }
        bytes += amount;
        length -= (size_t)amount;
    }
    return 1;
}

static uint32_t hashPixel(uint32_t hash, uint32_t pixel) {
    hash = xenosQuadHashByte(hash, (pixel >> 16) & 0xff);
    hash = xenosQuadHashByte(hash, (pixel >> 8) & 0xff);
    return xenosQuadHashByte(hash, pixel & 0xff);
}

static int clearAndRead(io_connect_t owner, unsigned handle, uint32_t color, Frame *frame) {
    unsigned ticket = 0;
    kern_return_t result = kIOReturnBusy;
    for (unsigned attempt = 0; attempt < 200 && result == kIOReturnBusy; ++attempt) {
        result = IOConnectMethodScalarIScalarO(owner, kXenosSurfaceClear, 2, 1, handle, color, &ticket);
        if (result == kIOReturnBusy)
            usleep(10000);
    }
    if (!expect("clear shared surface", result, KERN_SUCCESS) || !ticket)
        return 0;

    unsigned state = kXenosQuadPending, fence = 0, pixels = 0, mismatches = 0, driverHash = 0;
    for (unsigned attempt = 0; attempt < 200 && state == kXenosQuadPending; ++attempt) {
        result = IOConnectMethodScalarIScalarO(owner, kXenosQuadUploadStatus, 1,
            kXenosQuadUploadResultWords, ticket, &state, &fence, &pixels, &mismatches, &driverHash);
        if (!expect("read GPU fence status", result, KERN_SUCCESS))
            return 0;
        if (state == kXenosQuadPending)
            usleep(10000);
    }
    if (state != kXenosQuadPassed || fence != ticket || pixels != kXenosQuadReadbackPixels || mismatches) {
        fprintf(stderr, "FAIL ticket=%u state=%u fence=%u pixels=%u mismatches=%u\n",
                ticket, state, fence, pixels, mismatches);
        return 0;
    }

    uint32_t hash = XENOS_QUAD_HASH_INITIAL;
    for (unsigned row = 0; row < kXenosSurfaceSide; row += kXenosSurfaceReadRows) {
        XenosSurfaceReadRequest request = {handle, ticket, row};
        XenosSurfaceReadback readback;
        IOByteCount bytes = sizeof(readback);
        if (!expect("read owner pixels", IOConnectMethodStructureIStructureO(owner, kXenosSurfaceRead,
                   sizeof(request), &bytes, &request, &readback), KERN_SUCCESS) || bytes != sizeof(readback))
            return 0;
        for (unsigned i = 0; i < kXenosSurfaceReadPixels; ++i) {
            mismatches += readback.argb[i] != color;
            hash = hashPixel(hash, readback.argb[i]);
        }
    }
    if (mismatches || hash != driverHash) {
        fprintf(stderr, "FAIL owner readback ticket=%u mismatches=%u hash=%08x driver_hash=%08x\n",
                ticket, mismatches, (unsigned)hash, driverHash);
        return 0;
    }
    frame->ticket = ticket;
    frame->color = color;
    frame->hash = hash;
    printf("PASS owner pid=%ld handle=%u ticket=%u fence=%u argb=%08x pixels=%u hash=%08x\n",
           (long)getpid(), handle, ticket, fence, (unsigned)color, pixels, (unsigned)hash);
    return 1;
}

static kern_return_t importDrawable(io_connect_t reader, const XenosDrawableToken *token) {
    return IOConnectMethodScalarIScalarO(reader, kXenosDrawableImport, kXenosDrawableTokenWords, 0,
        token->words[0], token->words[1], token->words[2], token->words[3]);
}

static int rejectReplay(const XenosDrawableToken *token) {
    const io_connect_t replay = openClient(kXenosDrawableReaderType);
    if (replay == MACH_PORT_NULL)
        return 0;
    const int rejected = expect("reject a consumed token", importDrawable(replay, token),
                                kIOReturnNotPermitted);
    IOServiceClose(replay);
    return rejected;
}

static int runReader(int fd) {
    XenosDrawableToken token;
    if (!transfer(fd, &token, sizeof(token), 0))
        return 0;
    const io_connect_t reader = openClient(kXenosDrawableReaderType);
    if (reader == MACH_PORT_NULL)
        return 0;
    XenosDrawableToken wrong = token;
    wrong.words[0] ^= 1;
    int passed = expect("reject the wrong token", importDrawable(reader, &wrong), kIOReturnNotPermitted) &&
                 expect("import drawable", importDrawable(reader, &token), KERN_SUCCESS) &&
                 rejectReplay(&token);
    vm_address_t address = 0;
    vm_size_t length = 0;
    int mapped = 0;
    if (passed) {
        mapped = expect("map drawable", IOConnectMapMemory(reader, kXenosDrawableMemoryType,
                        mach_task_self(), &address, &length, kIOMapAnywhere), KERN_SUCCESS);
        passed = mapped && length == kPixelPageBytes;
        if (mapped && !passed)
            fprintf(stderr, "Mapped %lu bytes, expected %u\n", (unsigned long)length, kPixelPageBytes);
    }
    if (passed)
        passed = expect("reject making the mapping writable",
            vm_protect(mach_task_self(), address, length, 0, VM_PROT_READ | VM_PROT_WRITE),
            KERN_PROTECTION_FAILURE);
    const volatile uint32_t *pixels = (const volatile uint32_t *)address;
    for (unsigned round = 0; passed && round < kRounds; ++round) {
        Frame frame;
        if (!transfer(fd, &frame, sizeof(frame), 0)) {
            passed = 0;
            break;
        }
        ReaderResult observation = {(uint32_t)getpid(), frame.ticket, kXenosQuadReadbackPixels,
                                    0, XENOS_QUAD_HASH_INITIAL};
        for (unsigned i = 0; i < kXenosQuadReadbackPixels; ++i) {
            const uint32_t pixel = pixels[i];
            if (pixel != frame.color && !observation.mismatches)
                fprintf(stderr, "Mapped word %u is %08x, expected %08x\n",
                        i, (unsigned)pixel, (unsigned)frame.color);
            observation.mismatches += pixel != frame.color;
            observation.hash = hashPixel(observation.hash, pixel);
        }
        passed = transfer(fd, &observation, sizeof(observation), 1) &&
                 !observation.mismatches && observation.hash == frame.hash;
    }
    if (mapped)
        passed = expect("unmap drawable", IOConnectUnmapMemory(reader, kXenosDrawableMemoryType,
                        mach_task_self(), address), KERN_SUCCESS) && passed;
    IOServiceClose(reader);
    return passed;
}

static int runOwner(int fd, pid_t readerPid) {
    const io_connect_t owner = openClient(0);
    if (owner == MACH_PORT_NULL)
        return 0;
    unsigned handle = 0;
    int passed = expect("create drawable", IOConnectMethodScalarIScalarO(owner, kXenosSurfaceCreate,
                            2, 1, (unsigned)kXenosSurfaceSide, (unsigned)kXenosSurfaceSide, &handle),
                        KERN_SUCCESS) && handle;
    const uint32_t colors[kRounds] = {0xff4cb237, 0xff2148ce};
    unsigned previousTicket = 0;
    for (unsigned round = 0; passed && round < kRounds; ++round) {
        Frame frame;
        passed = clearAndRead(owner, handle, colors[round], &frame) &&
                 frame.ticket != previousTicket;
        if (!passed)
            break;
        previousTicket = frame.ticket;
        if (round == 0) {
            XenosDrawableToken token;
            passed = expect("export drawable", IOConnectMethodScalarIScalarO(owner, kXenosDrawableExport,
                          1, kXenosDrawableTokenWords, handle, &token.words[0], &token.words[1],
                          &token.words[2], &token.words[3]), KERN_SUCCESS) &&
                     transfer(fd, &token, sizeof(token), 1);
        }
        ReaderResult observed;
        if (passed)
            passed = transfer(fd, &frame, sizeof(frame), 1) &&
                     transfer(fd, &observed, sizeof(observed), 0);
        if (!passed) {
            fputs("The reader did not return its pixel check.\n", stderr);
            break;
        }
        passed = observed.pid == (uint32_t)readerPid && observed.pid != (uint32_t)getpid() &&
                 observed.ticket == frame.ticket && observed.pixels == kXenosQuadReadbackPixels &&
                 !observed.mismatches && observed.hash == frame.hash;
        printf("%s reader pid=%u ticket=%u pixels=%u mismatches=%u hash=%08x\n",
               passed ? "PASS" : "FAIL", (unsigned)observed.pid, (unsigned)observed.ticket,
               (unsigned)observed.pixels, (unsigned)observed.mismatches, (unsigned)observed.hash);
    }
    if (handle)
        passed = expect("release drawable", IOConnectMethodScalarIScalarO(owner, kXenosSurfaceRelease,
                        1, 0, handle), KERN_SUCCESS) && passed;
    IOServiceClose(owner);
    return passed;
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        puts("XenosSharedDrawable 1");
        return 0;
    }
    if (argc != 1) {
        fprintf(stderr, "Usage: %s [--version]\n", argv[0]);
        return 2;
    }

    int ends[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, ends) != 0) {
        perror("socketpair");
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);
    const pid_t child = fork();
    if (child == 0) {
        close(ends[0]);
        alarm(20);
        const int passed = runReader(ends[1]);
        close(ends[1]);
        _exit(passed ? 0 : 1);
    }

    close(ends[1]);
    if (child < 0) {
        perror("fork");
        close(ends[0]);
        return 1;
    }

    alarm(20);
    const int ownerPassed = runOwner(ends[0], child);
    close(ends[0]);
    int status;
    pid_t waited;
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    alarm(0);
    if (!ownerPassed || waited != child || !WIFEXITED(status) || WEXITSTATUS(status))
        return 1;
    puts("PASS shared_drawable processes=2 clears=2 gpu_pixels=2048 reader_pixels=2048 "
         "mapped_bytes=4096 read_only=1 wrong_token_rejected=1 replay_rejected=1");
    return 0;
}
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
