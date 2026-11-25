/*
 * xenon-audio-set: sets XenonAudio's test switches (builds with the XenonAudioTestKnobs
 * personality key). Must run as root.
 *
 *   xenon-audio-set ack 8|28          acknowledge bits in interrupt mode (0x08 or 0x1C, decimal)
 *   xenon-audio-set posfix 0|1        position from the state register (1) or the original code (0)
 *   xenon-audio-set reset 0|1         restart the DMA channel at descriptor 0 on every engine start
 *   xenon-audio-set interrupt 0|1     interrupt mode (1) or polling timer (0), from the next start
 *
 * Build on Mac OS X 10.4 or later:
 *   gcc -arch ppc -mmacosx-version-min=10.4 -isysroot /Developer/SDKs/MacOSX10.4u.sdk \
 *       -framework IOKit -framework CoreFoundation -o xenon-audio-set xenon-audio-set.c
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
  CFStringRef key;
  io_service_t audio;
  int value;

  if (argc != 3) {
    fprintf(stderr, "usage: %s ack|posfix|reset|interrupt <number>\n", argv[0]);
    return 2;
  }
  if (strcmp(argv[1], "ack") == 0) {
    key = CFSTR("XenonAudioAck");
  } else if (strcmp(argv[1], "posfix") == 0) {
    key = CFSTR("XenonAudioPositionFix");
  } else if (strcmp(argv[1], "reset") == 0) {
    key = CFSTR("XenonAudioResetOnStart");
  } else if (strcmp(argv[1], "interrupt") == 0) {
    key = CFSTR("XenonAudioUseInterrupt");
  } else {
    fprintf(stderr, "unknown switch %s\n", argv[1]);
    return 2;
  }
  value = atoi(argv[2]);

  audio = IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceMatching("XenonAudioDevice"));
  if (audio == 0) {
    fprintf(stderr, "XenonAudioDevice not found\n");
    return 1;
  }

  CFNumberRef number = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &value);
  CFMutableDictionaryRef dict = CFDictionaryCreateMutable(kCFAllocatorDefault, 1, &kCFTypeDictionaryKeyCallBacks,
                                                          &kCFTypeDictionaryValueCallBacks);
  CFDictionarySetValue(dict, key, number);
  kern_return_t kr = IORegistryEntrySetCFProperties(audio, dict);
  printf("%s = %d: 0x%x\n", argv[1], value, kr);

  CFRelease(dict);
  CFRelease(number);
  IOObjectRelease(audio);
  return (kr == KERN_SUCCESS) ? 0 : 1;
}
