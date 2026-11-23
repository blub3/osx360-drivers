## Native HDMI follow-up test

I tested a separate `XenonFramebufferTest` 0.0.5d1 build on my Xbox 360 Slim running Mac OS X 10.4.11 from a USB 2.0 SSD. The native build changes the HDMI timing and reapplies the transmitter’s PCM setup on each switch.

- Tiger exposed both display modes and switched 1920 × 1080 → 1280 × 720 → 1920 × 1080 without rebooting. Finder reported both desktop sizes, and my TV’s input information showed **720p** and **1080p** as the mode changed.
- I heard the same short iTunes clip over HDMI in both modes. The driver also read back `0x11` from the PCM enable register after each switch.

The native test is published separately from the scaler build in this PR. [Native HDMI driver and build notes](https://github.com/blub3/osx360-drivers/tree/xenon-native-hdmi-test/XenonFramebufferTest) · [Companion OpenBIOS branch](https://github.com/blub3/openbios/tree/codex/native-hdmi-test)
