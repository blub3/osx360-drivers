# Unarmed WindowServer copy-region test

`gpuwinauto` selects `XenosWinAuto.plugin` through the framebuffer
personality. It reuses the nine `gpuwintile` kext executables, including
XenonCoreGraphics 0.0.5d48 and XenonFramebufferTest 0.0.5d3. The candidate
OpenBIOS ELF changes the embedded archive name from `XWTC.MKE` to `XWAU.MKE`.
The test plug-in has its own bundle identifier and factory UUID.

`gpuwinoverlap` selects `XenosWinOverlap.plugin` with the same nine kext
executables. It accepts one overlapping copy rectangle with 1,025–8,192
clipped pixels. The first tile falls back immediately if the queue is
busy. Between tiles, it allows up to 200 ms of busy retries to account
for the driver's eight 15 ms cooldown ticks. The 1,500 ms capture
budget, source and destination recheck, and ordered fences apply to the
whole region. The callback publishes the region only after all tiles pass.

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

For `gpuwinoverlap`, pass `--overlap` to `build.sh`, `package.py`, and
`stage.sh`. Use a verified `gpuwinauto` volume copy as the parent. The
alternate menu is `kboot-gpu-window-unarmed-overlap.conf`. It retains
the `gpuwinauto` and `gpuwintile` entries and stages beside the active menu.

## Verify

On the working `gpuwintile` boot, the plug-in factory and timing checks can
run with `RunXenosWindowUnarmed.sh --plugin-control`. The
`CheckUnarmedOffscreen` executable uses a private 64 × 64 buffer to verify
one overlapping 324-pixel GPU copy, the surrounding pixels, and rejection
of a rectangle larger than one tile.

`CheckUnarmedOverlap` checks eight tiles, all 7,252 clipped pixels and
21,420 neighboring guard pixels in a private overlapping copy. It also
checks that oversized and non-overlapping copies leave the buffer intact.
On a candidate cold boot, the runner in `XenosWindowOverlapTest` checks
two overlapping window moves. For each move it requires eight ordered
kernel fence receipts, all 7,252 clipped pixels, and an independent
6,912-pixel display readback.

For the staged build, run `install-plugin-remote.sh` as root on Tiger,
then run `activate-remote.sh` to verify the protected USB files and save
the previous menu before selecting the candidate. Pass `overlap` to both
scripts for `gpuwinoverlap`. With no argument they select `gpuwinauto`.
After a cold boot, run `RunXenosWindowUnarmed.sh --repaint` over SSH from the staged
test directory. It requests two ordinary Carbon window moves and checks
for distinct increasing fences from the running WindowServer, matching
GPU and clipped framebuffer readbacks and independently sampled display
pixels. It verifies the old XenosWinTile arm and lock files are absent.
