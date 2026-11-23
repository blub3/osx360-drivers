#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
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
