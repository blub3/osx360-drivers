#include "TigerAPI.h"

extern unsigned long strtoul(const char *, char **, int);
extern UInt32 mach_task_self_;
extern int task_for_pid(UInt32, int, UInt32 *);
extern int vm_read_overwrite(UInt32, UInt32, UInt32, UInt32, UInt32 *);
extern int mach_port_deallocate(UInt32, UInt32);

static int readWord(UInt32 task, UInt32 address, UInt32 *word)
{
    UInt32 count = 0;
    return !vm_read_overwrite(task, address, sizeof(*word), (UInt32)word, &count) &&
        count == sizeof(*word);
}

int main(int argc, char **argv)
{
    char *end = 0;
    if (argc != 2)
        return 2;
    unsigned long pid = strtoul(argv[1], &end, 10);
    if (!pid || *end || pid > 0x7fffffffUL)
        return 2;
    UInt32 self = mach_task_self_, server = 0;
    int status = task_for_pid(self, (int)pid, &server);
    printf("WindowServer PID=%lu task_for_pid=%d\n", pid, status);
    if (status || !server)
        return 1;
    UInt32 signature;
    int ok = readWord(server, 0x903a9530, &signature) && signature == 0x7c0802a6;
    if (ok) {
        const struct {
            const char *name;
            UInt32 address;
        } checkpoints[] = {
            {"tile_width", 0xa0374028},
            {"tile_height", 0xa037402c},
            {"minimum_display_depth", 0xa0374024},
            {"compositor_context", 0xa03905e4},
            {"compositor_formats", 0xa03905e8},
            {"display_context_count", 0xa03905f4},
            {"display_contexts", 0xa0390614},
            {"cgls_display_count", 0xa0390b04},
            {"cgls_services", 0xa0390b08},
            {"cgls_plugin", 0xa0390b14},
        };
        for (unsigned i = 0; i < sizeof(checkpoints)/sizeof(checkpoints[0]); ++i) {
            UInt32 value = 0;
            if (readWord(server, checkpoints[i].address, &value))
                printf("%s=0x%08x (%u)\n", checkpoints[i].name, value, value);
            else
                ok = 0;
        }
    } else {
        printf("WindowServer CoreGraphics ABI signature did not match Tiger.\n");
    }
    mach_port_deallocate(self, server);
    return !ok;
}
