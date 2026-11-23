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
        fputs("XenosController was not found. Boot the multi-client queue test.\n", stderr);
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

static int waitAndRead(io_connect_t owner, unsigned handle, unsigned ticket, uint32_t color) {
    kern_return_t result;
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
    printf("PASS owner pid=%ld handle=%u ticket=%u fence=%u argb=%08x pixels=%u hash=%08x\n",
           (long)getpid(), handle, ticket, fence, (unsigned)color, pixels, (unsigned)hash);
    return 1;
}

typedef struct {
    unsigned pid, handle, ticket, ahead;
} Submission;

enum { kCreate = 1, kPair, kExitAfterEnqueue };
static const uint32_t kWarmupColor = 0xff4cb237;
static const uint32_t kParentColor = 0xffd14285;
static const uint32_t kWorkerColor = 0xff2148ce;
static const uint32_t kExitColor = 0xffa05d23;
static const uint32_t kSurvivorColor = 0xff1cc074;

static int command(int fd, unsigned operation) {
    return transfer(fd, &operation, sizeof(operation), 1);
}

static int receiveCommand(int fd, unsigned expected) {
    unsigned operation;
    return transfer(fd, &operation, sizeof(operation), 0) && operation == expected;
}

static int createSurface(io_connect_t connection, Submission *submission) {
    submission->pid = (unsigned)getpid();
    return expect("create queue surface", IOConnectMethodScalarIScalarO(connection, kXenosSurfaceCreate,
                  2, 1, (unsigned)kXenosSurfaceSide, (unsigned)kXenosSurfaceSide, &submission->handle),
                  KERN_SUCCESS) && submission->handle;
}

static int enqueue(io_connect_t connection, uint32_t color, Submission *submission) {
    submission->ticket = submission->ahead = 0;
    if (!expect("enqueue surface clear", IOConnectMethodScalarIScalarO(connection, kXenosSurfaceEnqueue,
                2, 2, submission->handle, color, &submission->ticket, &submission->ahead),
                KERN_SUCCESS) || !submission->ticket)
        return 0;
    printf("QUEUED pid=%u handle=%u ticket=%u ahead=%u\n",
           submission->pid, submission->handle, submission->ticket, submission->ahead);
    return 1;
}

static int readQueue(io_connect_t connection, unsigned ticket, XenosSurfaceQueueInfo *info) {
    IOByteCount bytes = sizeof(*info);
    return expect("read surface queue", IOConnectMethodStructureIStructureO(connection,
                  kXenosSurfaceQueueStatus, sizeof(ticket), &bytes, &ticket, info), KERN_SUCCESS) &&
           bytes == sizeof(*info);
}

static int verifiedFence(io_connect_t connection, const Submission *job, XenosSurfaceQueueInfo *info) {
    if (!readQueue(connection, job->ticket, info))
        return 0;
    if (info->state != kXenosSurfaceQueueComplete || info->fence != job->ticket ||
        info->handle != job->handle || !info->completion) {
        fprintf(stderr, "Ticket %u state=%u fence=%u handle=%u completion=%u\n",
                job->ticket, info->state, info->fence, info->handle, info->completion);
        return 0;
    }
    return 1;
}

static int runWorker(int fd) {
    if (!receiveCommand(fd, kCreate))
        return 0;
    const io_connect_t worker = openClient(0);
    if (worker == MACH_PORT_NULL)
        return 0;
    Submission pending = {0};
    XenosSurfaceQueueInfo info;
    const int passed = createSurface(worker, &pending) &&
        transfer(fd, &pending, sizeof(pending), 1) &&
        receiveCommand(fd, kPair) &&
        enqueue(worker, kWorkerColor, &pending) &&
        transfer(fd, &pending, sizeof(pending), 1) &&
        waitAndRead(worker, pending.handle, pending.ticket, kWorkerColor) &&
        verifiedFence(worker, &pending, &info) &&
        transfer(fd, &info.completion, sizeof(info.completion), 1) &&
        receiveCommand(fd, kExitAfterEnqueue) &&
        enqueue(worker, kExitColor, &pending) &&
        transfer(fd, &pending, sizeof(pending), 1);
    // The caller exits directly. The kernel must clean up the connection.
    return passed;
}

static int verifyExit(io_connect_t owner, const Submission *departed, uint32_t expectedLive,
                      uint32_t lastWorkerCompletion, uint32_t survivorCompletion) {
    XenosSurfaceQueueInfo info;
    for (unsigned attempt = 0; attempt < 200; ++attempt) {
        if (!readQueue(owner, departed->ticket, &info))
            return 0;
        if (info.state == kXenosSurfaceQueueFailed)
            break;
        if (info.ownerClosed && info.liveSurfaces == expectedLive && !info.pending && !info.activeTicket) {
            const int cancelled = info.state == kXenosSurfaceQueueCancelled &&
                                  !info.fence && !info.completion;
            const int drained = info.state == kXenosSurfaceQueueComplete &&
                                info.fence == departed->ticket &&
                                info.completion > lastWorkerCompletion &&
                                info.completion < survivorCompletion;
            if (info.handle == departed->handle && (cancelled || drained)) {
                printf("PASS exit ticket=%u handle=%u state=%s fence=%u completion=%u "
                       "owners=%u pending=0 active=0\n",
                       departed->ticket, departed->handle, cancelled ? "cancelled" : "drained",
                       info.fence, info.completion, info.liveSurfaces);
                return 1;
            }
            break;
        }
        usleep(10000);
    }
    fprintf(stderr, "Exit cleanup failed for ticket=%u state=%u owner_closed=%u owners=%u "
                    "pending=%u active=%u fence=%u completion=%u\n",
            departed->ticket, info.state, info.ownerClosed, info.liveSurfaces,
            info.pending, info.activeTicket, info.fence, info.completion);
    return 0;
}

static int runOwner(int fd, pid_t workerPid) {
    const io_connect_t owner = openClient(0);
    if (owner == MACH_PORT_NULL)
        return 0;
    XenosSurfaceQueueInfo initial, live, a, b, survivor;
    Submission parent = {0}, worker = {0}, departing = {0};
    uint32_t workerCompletion = 0;
    int passed = readQueue(owner, 0, &initial) && !initial.pending && !initial.activeTicket &&
        createSurface(owner, &parent) && command(fd, kCreate) &&
        transfer(fd, &worker, sizeof(worker), 0) && worker.pid == (unsigned)workerPid &&
        worker.pid != parent.pid && worker.handle && worker.handle != parent.handle &&
        readQueue(owner, 0, &live) && live.liveSurfaces == initial.liveSurfaces + 2;

    // A completed warm-up clear leaves a short scheduler cooldown. This
    // gives both processes time to enqueue, with a queue slot ahead of B.
    if (passed)
        passed = enqueue(owner, kWarmupColor, &parent) &&
                 waitAndRead(owner, parent.handle, parent.ticket, kWarmupColor) &&
                 enqueue(owner, kParentColor, &parent) && command(fd, kPair);
    Submission paired;
    if (passed)
        passed = transfer(fd, &paired, sizeof(paired), 0) &&
                 paired.pid == worker.pid && paired.handle == worker.handle && paired.ahead > 0 &&
                 waitAndRead(owner, parent.handle, parent.ticket, kParentColor) &&
                 transfer(fd, &workerCompletion, sizeof(workerCompletion), 0) &&
                 verifiedFence(owner, &parent, &a) && verifiedFence(owner, &paired, &b) &&
                 b.completion == workerCompletion && a.completion < b.completion;
    if (passed) {
        printf("PASS fifo first_ticket=%u first_completion=%u second_ticket=%u "
               "second_completion=%u second_ahead=%u\n",
               parent.ticket, a.completion, paired.ticket, b.completion, paired.ahead);
        passed = command(fd, kExitAfterEnqueue) &&
                 transfer(fd, &departing, sizeof(departing), 0) &&
                 departing.pid == worker.pid && departing.handle == worker.handle &&
                 departing.ticket && departing.ticket != paired.ticket &&
                 enqueue(owner, kSurvivorColor, &parent) &&
                 waitAndRead(owner, parent.handle, parent.ticket, kSurvivorColor) &&
                 verifiedFence(owner, &parent, &survivor) && survivor.completion > b.completion &&
                 verifyExit(owner, &departing, initial.liveSurfaces + 1, b.completion, survivor.completion);
    }
    if (parent.handle) {
        const int released = expect("release surviving surface", IOConnectMethodScalarIScalarO(owner,
                                     kXenosSurfaceRelease, 1, 0, parent.handle), KERN_SUCCESS);
        passed = passed && released && readQueue(owner, 0, &live) &&
                 live.liveSurfaces == initial.liveSurfaces && !live.pending && !live.activeTicket;
    }
    if (passed)
        printf("PASS buffers initial=%u final=%u pending=0 active=0\n",
               initial.liveSurfaces, live.liveSurfaces);
    else
        fputs("Multi-client fence or buffer check failed.\n", stderr);
    IOServiceClose(owner);
    return passed;
}
int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        puts("XenosMultiClient 1");
        return 0;
    }
    if (argc != 1) {
        fprintf(stderr, "Usage: %s [--version]\n", argv[0]);
        return 2;
    }

    setvbuf(stdout, NULL, _IOLBF, 0);
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
        const int passed = runWorker(ends[1]);
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
    puts("PASS multi_client processes=2 verified_clears=4 pixels=4096 fifo=1 exit_cleanup=1");
    return 0;
}
