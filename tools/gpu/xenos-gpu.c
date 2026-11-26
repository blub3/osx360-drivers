/*
 * xenos-gpu: user-space debug tool for XenosGPU (GPU plan steps 1.2 and 1.3, docs/gpu-driver-plan.md).
 * Most commands use XenosGPU's root-only debug client; "mem" uses the GPU client (type 1).
 *
 *   xenos-gpu read <offset>              one GPU register (byte offset, hex)
 *   xenos-gpu regs <offset> <count>      a range of registers, 4 bytes apart
 *   xenos-gpu write <offset> <value>     an interrupt register (CP/RBBM_INT_CNTL/ACK only)
 *   xenos-gpu fence [count]              submit fences, wait for each (polling), report latency
 *   xenos-gpu stats                      frames, busy ticks, fences, interrupts
 *   xenos-gpu vblank                     look for the VBlank bit and frame counter in the display block
 *   xenos-gpu irq on|off                 register or remove the GPU interrupt (step 1.2b; see the plan)
 *
 * GPU memory (step 1.3, client type 1, any user; "mem test" also needs root for the GPU write):
 *   xenos-gpu mem stats                  heap size, free, allocations, largest free block
 *   xenos-gpu mem test                   allocate, map, CPU and GPU write/read, ownership, errors, free
 *   xenos-gpu mem leak                   allocate and map, then exit without freeing (the driver frees)
 *   xenos-gpu mem exhaust                allocate until the heap and the handle table are full, free all
 *
 * Build on the G5 (Mac OS X 10.5 with the 10.4 SDK):
 *   gcc -std=c99 -arch ppc -mmacosx-version-min=10.4 -isysroot /Developer/SDKs/MacOSX10.4u.sdk -I XenosGPU/src \
 *       -framework IOKit -framework CoreFoundation -o xenos-gpu tools/gpu/xenos-gpu.c
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>
#include "XenosGPUDebugProtocol.h"
#include "XenosGPUProtocol.h"

static io_connect_t gConnection;

static double now(void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return tv.tv_sec + tv.tv_usec / 1e6;
}

static int open_client(unsigned type, io_connect_t *connection, int quiet) {
  io_service_t service = IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceMatching("XenosGPU"));
  if (service == 0) {
    fprintf(stderr, "XenosGPU is not loaded\n");
    return 0;
  }
  kern_return_t kr = IOServiceOpen(service, mach_task_self(), type, connection);
  IOObjectRelease(service);
  if (kr != KERN_SUCCESS) {
    if (!quiet) fprintf(stderr, "IOServiceOpen type %u failed: 0x%08x%s\n", type, kr, type == 0 ? " (run as root)" : "");
    return 0;
  }
  return 1;
}

static int open_gpu(void) {
  return open_client(kXenosGPUDebugClientType, &gConnection, 0);
}

static int read_reg(unsigned offset, unsigned *value) {
  kern_return_t kr = IOConnectMethodScalarIScalarO(gConnection, kXenosGPUDebugReadRegister, 1, 1, offset, value);
  if (kr != KERN_SUCCESS) {
    fprintf(stderr, "read 0x%04x failed: 0x%08x\n", offset, kr);
    return 0;
  }
  return 1;
}

static int cmd_fence(int count) {
  vm_address_t address = 0;
  vm_size_t size = 0;
  kern_return_t kr = IOConnectMapMemory(gConnection, kXenosGPUFenceMemoryType, mach_task_self(), &address,
                                        &size, kIOMapAnywhere);
  volatile unsigned *page = (kr == KERN_SUCCESS) ? (volatile unsigned *)address : NULL;
  if (page == NULL) {
    fprintf(stderr, "fence page map failed: 0x%08x\n", kr);
  }
  unsigned failures = 0, minimum = 0xFFFFFFFF, maximum = 0;
  double total = 0;
  for (int i = 0; i < count; i++) {
    unsigned sequence = 0, elapsed = 0, completed = 0;
    kr = IOConnectMethodScalarIScalarO(gConnection, kXenosGPUDebugSubmitFence, 0, 1, &sequence);
    if (kr != KERN_SUCCESS) {
      fprintf(stderr, "submit failed: 0x%08x\n", kr);
      return 1;
    }
    kr = IOConnectMethodScalarIScalarO(gConnection, kXenosGPUDebugWaitFence, 2, 2, sequence, 1000, &elapsed,
                                       &completed);
    unsigned mapped = page != NULL ? page[kXenosGPUFenceWordSequence] : 0;
    if (kr != KERN_SUCCESS || (int)(completed - sequence) < 0 || (page != NULL && (int)(mapped - sequence) < 0)) {
      failures++;
      printf("fence %u: FAIL (result 0x%08x, completed %u, mapped page %u)\n", sequence, kr, completed, mapped);
      continue;
    }
    total += elapsed;
    if (elapsed < minimum) minimum = elapsed;
    if (elapsed > maximum) maximum = elapsed;
    if (count <= 10) {
      printf("fence %u: completed after %u us (mapped page %u)\n", sequence, elapsed, mapped);
    }
  }
  printf("fences: %d submitted, %u failed, wait min %u us, average %.0f us, max %u us\n", count, failures,
         failures == (unsigned)count ? 0 : minimum, count > (int)failures ? total / (count - failures) : 0.0,
         maximum);
  return failures != 0;
}

/* Candidate display registers (R500/AVIVO layout, which the Xenos display block follows for D1GRPH). */
static const unsigned kDisplayCandidates[] = {
  0x6080, 0x6084, 0x6088, 0x608C, 0x6090, 0x6094, 0x6098, 0x609C, 0x60A0, 0x60A4, 0x60A8, 0x60AC,
  0x6528, 0x652C, 0x6530, 0x6534, 0x6538, 0x653C, 0x6540, 0x6544, 0x6548
};
#define kCandidates (sizeof(kDisplayCandidates) / sizeof(kDisplayCandidates[0]))

static int cmd_vblank(void) {
  unsigned first[kCandidates], last[kCandidates], previous[kCandidates], toggles[kCandidates][32];
  memset(toggles, 0, sizeof(toggles));
  for (unsigned i = 0; i < kCandidates; i++) {
    if (!read_reg(kDisplayCandidates[i], &first[i])) return 1;
    previous[i] = first[i];
  }
  double start = now();
  unsigned samples = 0;
  while (now() - start < 1.0) {
    for (unsigned i = 0; i < kCandidates; i++) {
      unsigned value;
      if (!read_reg(kDisplayCandidates[i], &value)) return 1;
      unsigned changed = value ^ previous[i];
      for (unsigned b = 0; b < 32; b++) {
        if (changed & (1U << b)) toggles[i][b]++;
      }
      previous[i] = value;
    }
    samples++;
  }
  double seconds = now() - start;
  for (unsigned i = 0; i < kCandidates; i++) last[i] = previous[i];
  printf("vblank: %u sample rounds in %.3f s (%.0f per second per register)\n", samples, seconds,
         samples / seconds);
  for (unsigned i = 0; i < kCandidates; i++) {
    printf("0x%04x first=0x%08x last=0x%08x delta=%d", kDisplayCandidates[i], first[i], last[i],
           (int)(last[i] - first[i]));
    for (unsigned b = 0; b < 32; b++) {
      if (toggles[i][b] != 0) {
        printf(" bit%u:%u", b, toggles[i][b]);
      }
    }
    printf("\n");
  }
  printf("A VBlank status bit toggles about 120 times per second at 60 Hz (on and off); a frame counter's\n"
         "delta is about 60.\n");
  return 0;
}

/* ---- GPU memory (step 1.3) ---- */

typedef struct { unsigned heap, free, allocations, largest; } heap_stats;

static int get_stats(io_connect_t c, heap_stats *s) {
  kern_return_t kr = IOConnectMethodScalarIScalarO(c, kXenosGPUHeapStatistics, 0, 4, &s->heap, &s->free,
                                                   &s->allocations, &s->largest);
  if (kr != KERN_SUCCESS) {
    fprintf(stderr, "heap statistics failed: 0x%08x\n", kr);
    return 0;
  }
  return 1;
}

static void print_stats(const char *label, const heap_stats *s) {
  printf("%s: heap %u KiB, free %u KiB, %u allocations, largest free %u KiB\n", label, s->heap / 1024,
         s->free / 1024, s->allocations, s->largest / 1024);
}

static kern_return_t mem_alloc(io_connect_t c, unsigned bytes, unsigned *handle, unsigned *gpu, unsigned *size) {
  *handle = *gpu = *size = 0;
  return IOConnectMethodScalarIScalarO(c, kXenosGPUAllocate, 2, 3, bytes, 0, handle, gpu, size);
}

static kern_return_t mem_free(io_connect_t c, unsigned handle) {
  return IOConnectMethodScalarIScalarO(c, kXenosGPUFree, 1, 0, handle);
}

static volatile unsigned *mem_map(io_connect_t c, unsigned handle, unsigned *mapped, kern_return_t *result) {
  vm_address_t address = 0;
  vm_size_t size = 0;
  kern_return_t kr = IOConnectMapMemory(c, handle, mach_task_self(), &address, &size, kIOMapAnywhere);
  if (result) *result = kr;
  if (mapped) *mapped = (unsigned)size;
  return kr == KERN_SUCCESS ? (volatile unsigned *)address : NULL;
}

static kern_return_t mem_unmap(io_connect_t c, unsigned handle, volatile unsigned *words) {
  return IOConnectUnmapMemory(c, handle, mach_task_self(), (vm_address_t)words);
}

static unsigned gFailures;
#define CHECK(cond, ...) do { if (cond) { printf("  ok:   "); } else { printf("  FAIL: "); gFailures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

static int gpu_write_and_fence(unsigned address, unsigned value) {
  unsigned sequence = 0, elapsed = 0, completed = 0;
  if (IOConnectMethodScalarIScalarO(gConnection, kXenosGPUDebugGPUWrite, 2, 0, address, value) != KERN_SUCCESS)
    return 0;
  if (IOConnectMethodScalarIScalarO(gConnection, kXenosGPUDebugSubmitFence, 0, 1, &sequence) != KERN_SUCCESS)
    return 0;
  return IOConnectMethodScalarIScalarO(gConnection, kXenosGPUDebugWaitFence, 2, 2, sequence, 1000, &elapsed,
                                       &completed) == KERN_SUCCESS && (int)(completed - sequence) >= 0;
}

#define kTestBlocks 6
static const unsigned kTestSizes[kTestBlocks] = { 1, 4096, 4097, 65536, 1 << 20, 16 << 20 };

static int cmd_mem_test(void) {
  io_connect_t c, other;
  if (!open_client(kXenosGPUClientType, &c, 0) || !open_client(kXenosGPUClientType, &other, 0)) return 1;
  int haveDebug = open_client(kXenosGPUDebugClientType, &gConnection, 1);
  heap_stats start, s;
  if (!get_stats(c, &start)) return 1;
  print_stats("start", &start);
  unsigned heapLow = 0xFFFFFFFF, heapHigh = 0;
  unsigned handle[kTestBlocks], gpu[kTestBlocks], size[kTestBlocks];
  volatile unsigned *words[kTestBlocks];

  printf("allocate and map:\n");
  for (int i = 0; i < kTestBlocks; i++) {
    unsigned want = (kTestSizes[i] + 4095) & ~4095U, mapped = 0;
    kern_return_t kr = mem_alloc(c, kTestSizes[i], &handle[i], &gpu[i], &size[i]);
    CHECK(kr == KERN_SUCCESS && handle[i] != 0 && size[i] == want && (gpu[i] & 4095) == 0,
          "allocate %u bytes: 0x%08x, handle %u, GPU 0x%08x, %u bytes", kTestSizes[i], kr, handle[i], gpu[i], size[i]);
    words[i] = mem_map(c, handle[i], &mapped, &kr);
    CHECK(words[i] != NULL && mapped == size[i], "map handle %u: 0x%08x, %u bytes at %p", handle[i], kr, mapped,
          (void *)words[i]);
    if (gpu[i] < heapLow) heapLow = gpu[i];
    if (gpu[i] + size[i] > heapHigh) heapHigh = gpu[i] + size[i];
  }
  int overlap = 0;
  for (int i = 0; i < kTestBlocks; i++)
    for (int j = i + 1; j < kTestBlocks; j++)
      if (gpu[i] < gpu[j] + size[j] && gpu[j] < gpu[i] + size[i]) overlap = 1;
  CHECK(!overlap, "no two allocations overlap (0x%08x-0x%08x)", heapLow, heapHigh);
  if (!get_stats(c, &s)) return 1;
  unsigned held = 0;
  for (int i = 0; i < kTestBlocks; i++) held += size[i];
  CHECK(s.allocations == start.allocations + kTestBlocks && s.free == start.free - held,
        "statistics: %u allocations, free %u KiB (expected %u KiB)", s.allocations, s.free / 1024,
        (start.free - held) / 1024);

  printf("zeroed, CPU write and read back:\n");
  double t0 = now();
  for (int i = 0; i < kTestBlocks; i++) {
    if (words[i] == NULL) continue;
    unsigned n = size[i] / 4, nonzero = 0, bad = 0;
    for (unsigned w = 0; w < n; w++) if (words[i][w] != 0) nonzero++;
    for (unsigned w = 0; w < n; w++) words[i][w] = gpu[i] + 4 * w;
    for (unsigned w = 0; w < n; w++) if (words[i][w] != gpu[i] + 4 * w) bad++;
    CHECK(nonzero == 0 && bad == 0, "handle %u: %u words, %u not zero, %u wrong after write", handle[i], n,
          nonzero, bad);
  }
  printf("  (%.2f s for %u KiB uncached, three passes)\n", now() - t0, held / 1024);

  printf("GPU write (CP_MEM_WRITE), fence, CPU read:\n");
  if (!haveDebug) {
    printf("  FAIL: debug client not available (run as root)\n");
    gFailures++;
  } else {
    for (int i = 0; i < kTestBlocks; i++) {
      if (words[i] == NULL) continue;
      unsigned last = size[i] / 4 - 1, v0 = 0xC0DE0000 | handle[i], v1 = 0xFACE0000 | handle[i];
      int ok = gpu_write_and_fence(gpu[i], v0) && gpu_write_and_fence(gpu[i] + 4 * last, v1);
      CHECK(ok && words[i][0] == v0 && words[i][last] == v1 && words[i][1] == gpu[i] + 4,
            "handle %u: first 0x%08x (want 0x%08x), last 0x%08x (want 0x%08x), neighbour 0x%08x", handle[i],
            words[i][0], v0, words[i][last], v1, words[i][1]);
    }
    /* The last word of XenosGPU's own 64 KiB (boot-mark page, unused there): harmless even if the check
       failed. No test above the heap: that is Tiger RAM (0x10000000). */
    unsigned below = 0x08210000 - 4;
    kern_return_t kr = IOConnectMethodScalarIScalarO(gConnection, kXenosGPUDebugGPUWrite, 2, 0, below, 0);
    CHECK(kr != KERN_SUCCESS, "GPU write below the managed heap (0x%08x) refused: 0x%08x", below, kr);
    kr = IOConnectMethodScalarIScalarO(gConnection, kXenosGPUDebugGPUWrite, 2, 0, gpu[0] + 2, 0);
    CHECK(kr != KERN_SUCCESS, "unaligned GPU write refused: 0x%08x", kr);
  }

  printf("ownership and errors:\n");
  unsigned h, g, sz, mapped;
  kern_return_t kr;
  CHECK(mem_free(other, handle[0]) != KERN_SUCCESS, "another client cannot free handle %u", handle[0]);
  CHECK(mem_map(other, handle[0], &mapped, &kr) == NULL, "another client cannot map handle %u: 0x%08x", handle[0], kr);
  CHECK(mem_map(c, 0x7FFFFFF0, &mapped, &kr) == NULL, "unknown handle cannot be mapped: 0x%08x", kr);
  CHECK(mem_free(c, 0x7FFFFFF0) != KERN_SUCCESS, "unknown handle cannot be freed");
  CHECK(mem_alloc(c, 0, &h, &g, &sz) != KERN_SUCCESS, "zero-byte allocation refused");
  kr = mem_alloc(c, start.heap + 4096, &h, &g, &sz);
  CHECK(kr != KERN_SUCCESS, "allocation larger than the heap refused: 0x%08x", kr);
  kr = IOConnectMethodScalarIScalarO(c, kXenosGPUAllocate, 2, 3, 4096, 1, &h, &g, &sz);
  CHECK(kr != KERN_SUCCESS, "unknown flags refused: 0x%08x", kr);
  kr = mem_free(c, handle[0]);
  CHECK(kr != KERN_SUCCESS, "free while still mapped refused: 0x%08x", kr);

  printf("unmap and free:\n");
  for (int i = 0; i < kTestBlocks; i++) {
    kern_return_t u = words[i] ? mem_unmap(c, handle[i], words[i]) : KERN_FAILURE;
    kr = mem_free(c, handle[i]);
    CHECK(u == KERN_SUCCESS && kr == KERN_SUCCESS, "handle %u: unmap 0x%08x, free 0x%08x", handle[i], u, kr);
  }
  CHECK(mem_free(c, handle[0]) != KERN_SUCCESS, "double free refused");
  kr = mem_alloc(c, 8192, &h, &g, &sz);
  volatile unsigned *again = kr == KERN_SUCCESS ? mem_map(c, h, &mapped, NULL) : NULL;
  CHECK(again != NULL && again[0] == 0 && again[1] == 0 && again[2047] == 0,
        "reused pages are zeroed again (GPU 0x%08x)", g);
  if (again) mem_unmap(c, h, again);
  if (kr == KERN_SUCCESS) mem_free(c, h);

  if (!get_stats(c, &s)) return 1;
  print_stats("end", &s);
  CHECK(s.free == start.free && s.allocations == start.allocations && s.largest == start.largest,
        "heap counters back at their start values");
  printf("mem test: %s (%u failures)\n", gFailures ? "FAIL" : "PASS", gFailures);
  IOServiceClose(other);
  IOServiceClose(c);
  if (haveDebug) IOServiceClose(gConnection);
  return gFailures != 0;
}

static int cmd_mem_leak(void) {
  io_connect_t c;
  if (!open_client(kXenosGPUClientType, &c, 0)) return 1;
  heap_stats s;
  unsigned h, g, sz, mapped;
  for (int i = 0; i < 3; i++) {
    kern_return_t kr = mem_alloc(c, (i + 1) << 20, &h, &g, &sz);
    volatile unsigned *w = kr == KERN_SUCCESS ? mem_map(c, h, &mapped, NULL) : NULL;
    if (w) w[0] = 0x1EA4;
    printf("leak: handle %u, GPU 0x%08x, %u KiB, %s\n", h, g, sz / 1024, w ? "mapped" : "NOT mapped");
  }
  if (get_stats(c, &s)) print_stats("holding", &s);
  printf("exiting without free, unmap or close; then run \"mem stats\" (expect the start values)\n");
  return 0;
}

static int cmd_mem_exhaust(void) {
  io_connect_t c;
  if (!open_client(kXenosGPUClientType, &c, 0)) return 1;
  heap_stats start, s;
  if (!get_stats(c, &start)) return 1;
  print_stats("start", &start);
  static unsigned handles[kXenosGPUMaxAllocations + 16];
  unsigned count = 0, g, sz;
  kern_return_t kr = KERN_SUCCESS;
  double t0 = now();
  while (count < kXenosGPUMaxAllocations + 16 && (kr = mem_alloc(c, 1 << 20, &handles[count], &g, &sz)) == KERN_SUCCESS)
    count++;
  get_stats(c, &s);
  printf("1 MiB blocks: %u allocated in %.2f s, then 0x%08x; free %u KiB, largest %u KiB\n", count, now() - t0, kr,
         s.free / 1024, s.largest / 1024);
  CHECK(kr != KERN_SUCCESS && s.largest < (1 << 20) && count == start.largest >> 20,
        "heap full after %u blocks (expected %u)", count, start.largest >> 20);
  for (unsigned i = 0; i < count; i++) mem_free(c, handles[i]);
  count = 0;
  while (count < kXenosGPUMaxAllocations + 16 && (kr = mem_alloc(c, 4096, &handles[count], &g, &sz)) == KERN_SUCCESS)
    count++;
  CHECK(kr != KERN_SUCCESS && count == kXenosGPUMaxAllocations - start.allocations,
        "handle table full after %u pages: 0x%08x (expected %u)", count, kr, kXenosGPUMaxAllocations - start.allocations);
  for (unsigned i = 0; i < count; i++) mem_free(c, handles[i]);
  get_stats(c, &s);
  print_stats("end", &s);
  CHECK(s.free == start.free && s.allocations == start.allocations, "heap counters back at their start values");
  printf("mem exhaust: %s (%u failures)\n", gFailures ? "FAIL" : "PASS", gFailures);
  IOServiceClose(c);
  return gFailures != 0;
}

static int cmd_mem(int argc, char **argv) {
  if (argc > 2 && !strcmp(argv[2], "test")) return cmd_mem_test();
  if (argc > 2 && !strcmp(argv[2], "leak")) return cmd_mem_leak();
  if (argc > 2 && !strcmp(argv[2], "exhaust")) return cmd_mem_exhaust();
  io_connect_t c;
  heap_stats s;
  if (!open_client(kXenosGPUClientType, &c, 0) || !get_stats(c, &s)) return 1;
  print_stats("heap", &s);
  IOServiceClose(c);
  return 0;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s read|regs|write|fence|stats|vblank|irq|mem ...\n", argv[0]);
    return 2;
  }
  if (!strcmp(argv[1], "mem")) return cmd_mem(argc, argv);
  if (!open_gpu()) return 1;
  int result = 0;
  if (!strcmp(argv[1], "read") && argc > 2) {
    unsigned value;
    unsigned offset = strtoul(argv[2], NULL, 16);
    result = !read_reg(offset, &value);
    if (!result) printf("0x%04x = 0x%08x\n", offset, value);
  } else if (!strcmp(argv[1], "regs") && argc > 3) {
    unsigned offset = strtoul(argv[2], NULL, 16), count = strtoul(argv[3], NULL, 0);
    for (unsigned i = 0; i < count && !result; i++) {
      unsigned value;
      result = !read_reg(offset + 4 * i, &value);
      if (!result) printf("0x%04x = 0x%08x\n", offset + 4 * i, value);
    }
  } else if (!strcmp(argv[1], "write") && argc > 3) {
    unsigned offset = strtoul(argv[2], NULL, 16), value = strtoul(argv[3], NULL, 16);
    kern_return_t kr = IOConnectMethodScalarIScalarO(gConnection, kXenosGPUDebugWriteRegister, 2, 0, offset, value);
    printf("write 0x%04x = 0x%08x: 0x%08x\n", offset, value, kr);
    result = kr != KERN_SUCCESS;
  } else if (!strcmp(argv[1], "fence")) {
    result = cmd_fence(argc > 2 ? atoi(argv[2]) : 10);
  } else if (!strcmp(argv[1], "stats")) {
    unsigned frames, busy, submitted, completed, interrupts, last;
    kern_return_t kr = IOConnectMethodScalarIScalarO(gConnection, kXenosGPUDebugStatistics, 0, 6, &frames, &busy,
                                                     &submitted, &completed, &interrupts, &last);
    printf("frames=%u busy_ticks=%u fences_submitted=%u fence_word=%u interrupts=%u last_cp_int_status=0x%08x (0x%08x)\n",
           frames, busy, submitted, completed, interrupts, last, kr);
    result = kr != KERN_SUCCESS;
  } else if (!strcmp(argv[1], "vblank")) {
    result = cmd_vblank();
  } else if (!strcmp(argv[1], "irq") && argc > 2) {
    unsigned enable = !strcmp(argv[2], "on");
    kern_return_t kr = IOConnectMethodScalarIScalarO(gConnection, kXenosGPUDebugInterrupt, 1, 0, enable);
    printf("irq %s: 0x%08x\n", enable ? "on" : "off", kr);
    result = kr != KERN_SUCCESS;
  } else {
    fprintf(stderr, "unknown command\n");
    result = 2;
  }
  IOServiceClose(gConnection);
  return result;
}
