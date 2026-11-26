/*
 * xenos-gpu: user-space debug tool for XenosGPU (GPU plan step 1.2, docs/gpu-driver-plan.md).
 * Talks to XenosGPU's root-only debug client. Must run as root while XenosGPU is loaded.
 *
 *   xenos-gpu read <offset>              one GPU register (byte offset, hex)
 *   xenos-gpu regs <offset> <count>      a range of registers, 4 bytes apart
 *   xenos-gpu write <offset> <value>     an interrupt register (CP/RBBM_INT_CNTL/ACK only)
 *   xenos-gpu fence [count]              submit fences, wait for each (polling), report latency
 *   xenos-gpu stats                      frames, busy ticks, fences, interrupts
 *   xenos-gpu vblank                     look for the VBlank bit and frame counter in the display block
 *   xenos-gpu irq on|off                 register or remove the GPU interrupt (step 1.2b; see the plan)
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

static io_connect_t gConnection;

static double now(void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return tv.tv_sec + tv.tv_usec / 1e6;
}

static int open_gpu(void) {
  io_service_t service = IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceMatching("XenosGPU"));
  if (service == 0) {
    fprintf(stderr, "XenosGPU is not loaded\n");
    return 0;
  }
  kern_return_t kr = IOServiceOpen(service, mach_task_self(), 0, &gConnection);
  IOObjectRelease(service);
  if (kr != KERN_SUCCESS) {
    fprintf(stderr, "IOServiceOpen failed: 0x%08x (run as root)\n", kr);
    return 0;
  }
  return 1;
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

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s read|regs|write|fence|stats|vblank|irq ...\n", argv[0]);
    return 2;
  }
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
