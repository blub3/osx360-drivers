# Unarmed WindowServer copy-region test

`gpuwinauto` selects `XenosWinAuto.plugin` through the framebuffer
personality. It reuses the nine `gpuwintile` kext executables, including
XenonCoreGraphics 0.0.5d48 and XenonFramebufferTest 0.0.5d3. The candidate
OpenBIOS ELF changes the embedded archive name from `XWTC.MKE` to `XWAU.MKE`.
The test plug-in has its own bundle identifier and factory UUID.

After an initial five-second grace period, the plug-in can accept a native
WindowServer framebuffer copy-region callback for one moved rectangle of
256 to 1,024 clipped pixels in the common 720p/1080p area. It makes one
GPU tile submission and waits for a matching fence and readback. It checks
both ends of the copy again before publishing the GPU RGB result, retains
source alpha and verifies the destination pixels. It leaves unsupported,
busy or changed regions to Tiger's software path before publishing. It
waits one second between attempts and abandons a completed capture that
exceeds the 350 ms capture budget. The callback still publishes the verified
pixels to scanout from the CPU. Quartz Extreme activation remains open.

## Build and stage

Use `build.sh <output-directory>` with the PowerPC cross toolchain and Tiger
reference inputs described in the [GPU work index](../../README.md). Override
their locations with `OSX360_TOOLCHAIN` and `OSX360_TIGER_REFERENCE` as needed.
Run `python3 package.py <verified-gpuwintile-volume-copy> <build-directory> <new-stage-directory>`.
The packager verifies the source firmware and archive against SHA-256
and checks every kext payload after replacing the plug-in selection.

Use `stage.sh <stage-directory> /Volumes/OSX360BOOT` with a locally mounted
stick, or pass an SSH wrapper for a stick mounted on Tiger. It copies the
candidate and a saved copy of the current menu alongside `kboot.conf` and
checks the payload and previously existing files. The source menu for a
future activation is `kboot-gpu-window-unarmed.conf`.

## Verify

On the working `gpuwintile` boot, the plug-in factory and timing checks can
run with `RunXenosWindowUnarmed.sh --plugin-control`. The
`CheckUnarmedOffscreen` executable uses a private 64 × 64 buffer to verify
one overlapping 324-pixel GPU copy, the surrounding pixels, and rejection
of a rectangle larger than one tile.

For the staged build, run `install-plugin-remote.sh` as root on Tiger to
install the verified `XenosWinAuto.plugin` alongside `XenosWinTile.plugin`.
Run `activate-remote.sh` on Tiger to save the working `gpuwintile` rollback,
select `gpuwinauto`, and verify the protected USB files. After a cold boot,
run `RunXenosWindowUnarmed.sh --repaint` over SSH from the staged
test directory. It requests two ordinary Carbon window moves and checks
for distinct increasing fences from the running WindowServer, matching
GPU and clipped framebuffer readbacks and independently sampled display
pixels. It verifies the old XenosWinTile arm and lock files are absent.
