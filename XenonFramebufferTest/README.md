# XenonFramebufferTest 0.0.5d1

This Xbox 360 Slim test changes the native HDMI output between 1280 × 720 at 60 Hz and 1920 × 1080 at 60 Hz from Tiger’s Displays settings. OpenBIOS captures the Slim’s initial 720p timing before bringing up 1080p. Tiger uses both timing profiles and reapplies the working HDMI PCM setup after each switch.

## Xbox 360 Slim test results

Tested on one Xbox 360 Slim running Mac OS X 10.4.11 from a USB 2.0 SSD, with a TV connected over HDMI.

- Tiger exposed both modes. The test switched 1920 × 1080 → 1280 × 720 → 1920 × 1080 in one boot. Finder reported the expected desktop bounds, and the TV’s input information reported 720p and 1080p as the mode changed.
- The same short iTunes clip was audible over HDMI in both modes. The driver read back `0x11` from the transmitter’s PCM enable register after each transition.
- The separate scaler test offers the same desktop sizes with a fixed 1080p HDMI signal. [Scaler test](https://github.com/blub3/osx360-drivers/tree/xenon-framebuffer-test/XenonFramebufferTest)

## Build and boot

Use the PowerPC toolchain and the `MacPPCKernelSDK` checkout at the repository root. See the [project build instructions](../BUILDING.md). The tested kexts were built with GCC 4.2.1, the classic PowerPC linker, and [MacPPCKernelSDK commit `12b9fac`](https://github.com/Goldfish64/MacPPCKernelSDK/tree/12b9fac01a05c4ceeb27e42af53c76b810c135e7).

```sh
make -C XenonFramebufferTest OSX_VERSION=tiger
make -C XenonSMC OSX_VERSION=tiger
```

Build the companion OpenBIOS branch for `xbox360-ppc`. On the boot volume, give its ELF a separate XeLL entry and place `NativeHD.mkext` beside it. The test archive contains the two kexts built above, the target Tiger installation’s `IOGraphicsFamily` 1.4.2, and the other working Xbox drivers, including `XenonCoreGraphics` and `XenonAudio`. Keep the existing boot entries and archives for rollback. [Companion OpenBIOS branch](https://github.com/blub3/openbios/tree/codex/native-hdmi-test)

Cold boot with the test entry, then select a mode in **System Preferences → Displays**. OpenBIOS publishes the profiles when it starts from the expected 720p HDMI signal and completes the boot-time transition to 1080p.
