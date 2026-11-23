## Summary

Add XenonFramebufferTest 0.0.4 as an experimental Tiger kext for the Xbox 360 Slim. It attaches to the inherited 1080p framebuffer and offers 1920 × 1080 and 1280 × 720 desktop modes. Xenos scales the 1280 × 720 desktop to fill the 1080p60 HDMI output. Both modes use the HDMI audio setup established by OpenBIOS.

The driver is built separately so it can be tried with an alternate startup mkext. The [driver README](https://github.com/blub3/osx360-drivers/blob/a154f5a559acee7f313a936efc45c83640da2dfd/XenonFramebufferTest/README.md) contains the build and boot instructions.

## Hardware test results

Tested on one Xbox 360 Slim running Mac OS X 10.4.11 from a USB 2.0 SSD, connected over HDMI. The test used the [OpenBIOS build with the 1080p HDMI audio setup](https://github.com/blub3/openbios/tree/859256055b918cfa69bb7b5b3d999bfb702b4b78).

- After a cold boot, the I/O Registry showed `display0` attached to `XenonFramebufferTest`.
- Quartz listed both resolutions as usable desktop modes. I switched 1920 × 1080 → 1280 × 720 → 1920 × 1080 without rebooting. Finder reported the expected desktop bounds in both modes, and the driver logged both transitions with HDMI timing at 1920 × 1080.
- iTunes music was audible over HDMI at both desktop resolutions. The scripted system alerts were inaudible, so the audio result comes from listening to iTunes playback.
