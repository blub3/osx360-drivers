/*
 * xenon-cpu-start: starts or stops a secondary CPU on OSX360 SMP builds.
 *   xenon-cpu-start <n>        sets IOCPUState = "Running" on logical CPU n; XenonCPU::setProperties
 *                              then calls processor_start if the CPU is stopped.
 *   xenon-cpu-start <n> stop   sets IOCPUState = "stopped"; Tiger's IOCPU::setProperty then calls
 *                              XenonCPU::haltCPU, which takes the CPU offline and parks it.
 * Must run as root.
 *
 * Build on Mac OS X 10.4 or later:
 *   gcc -arch ppc -mmacosx-version-min=10.4 -isysroot /Developer/SDKs/MacOSX10.4u.sdk \
 *       -framework IOKit -framework CoreFoundation -o xenon-cpu-start xenon-cpu-start.c
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
  io_iterator_t iter;
  io_object_t   cpu;
  int           wanted = (argc > 1) ? atoi(argv[1]) : 1;
  int           found = 0;
  int           stop = (argc > 2) && (strcmp(argv[2], "stop") == 0);
  CFStringRef   request = stop ? CFSTR("stopped") : CFSTR("Running");

  if (wanted < 1) {
    fprintf(stderr, "usage: %s <logical CPU number, 1 or higher> [stop]\n", argv[0]);
    return 2;
  }
  if (IOServiceGetMatchingServices(kIOMasterPortDefault, IOServiceMatching("XenonCPU"), &iter) != KERN_SUCCESS) {
    fprintf(stderr, "no XenonCPU services\n");
    return 1;
  }
  while ((cpu = IOIteratorNext(iter)) != 0) {
    CFNumberRef number = IORegistryEntryCreateCFProperty(cpu, CFSTR("IOCPUNumber"), kCFAllocatorDefault, 0);
    CFStringRef state  = IORegistryEntryCreateCFProperty(cpu, CFSTR("IOCPUState"), kCFAllocatorDefault, 0);
    int         value  = -1;
    char        stateText[32] = "?";

    if (number != NULL) {
      CFNumberGetValue(number, kCFNumberIntType, &value);
      CFRelease(number);
    }
    if (state != NULL) {
      CFStringGetCString(state, stateText, sizeof (stateText), kCFStringEncodingASCII);
      CFRelease(state);
    }
    printf("XenonCPU logical %d state %s\n", value, stateText);
    if (value == wanted) {
      kern_return_t kr = IORegistryEntrySetCFProperty(cpu, CFSTR("IOCPUState"), request);
      printf("set IOCPUState=%s on CPU %d: 0x%x\n", stop ? "stopped" : "Running", wanted, kr);
      found = 1;
    }
    IOObjectRelease(cpu);
  }
  IOObjectRelease(iter);
  return found ? 0 : 1;
}
