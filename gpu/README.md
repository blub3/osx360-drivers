# OSX360 GPU work

The source of truth is the `gpu/quartz-extreme` branch in
[blub3/osx360-drivers](https://github.com/blub3/osx360-drivers/tree/gpu/quartz-extreme)
and the corresponding
[OpenBIOS branch](https://github.com/blub3/openbios/tree/gpu/quartz-extreme).
See the [milestone tracker](../docs/gpu-milestones.md) for hardware evidence
and the [USB inventory](../docs/gpu-boot-profiles.md) for exact filenames,
version sets, source tags, and checksums.

The top-level `XenonCoreGraphics` and `XenonFramebufferTest` are the latest
tested sources, versions 0.0.5d48 and 0.0.5d3. The XenonSMC and native HDMI
driver work from commit `c4b62b4` is included through the fork's main branch.
The experiments under [lab/](lab/) preserve the source layout used by
historical scripts, including copies of earlier driver sources, app code,
bundle metadata, package scripts, and boot profiles.

## GLDs and plug-ins

| Component | Source and build recipe |
| --- | --- |
| GLDriverXenosNative | [gpu-native-gld](lab/gpu-native-gld/) |
| GLDriverXenosWSProbe | [gpu-windowserver-submit](lab/gpu-windowserver-submit/) |
| GLDriverXenosCompositor | [gpu-compositor-context](lab/gpu-compositor-context/) |
| GLDriverXenosCompositorRetry | [gpu-compositor-retry](lab/gpu-compositor-retry/) |
| XenosLegacyBlit | [gpu-fb-blit-surface](lab/gpu-fb-blit-surface/) |
| XenosBeamBlit | [gpu-fb-blit-beam](lab/gpu-fb-blit-beam/) |
| XenosWinBlit | [gpu-window-repaint](lab/gpu-window-repaint/) |
| XenosWinFix | [gpu-window-repaint-retry](lab/gpu-window-repaint-retry/) |
| XenosWinMap | [gpu-window-framebuffer-map](lab/gpu-window-framebuffer-map/) |
| XenosWinAll | [gpu-window-full-region](lab/gpu-window-full-region/) |
| XenosWinBatch | [gpu-window-batched-overlap](lab/gpu-window-batched-overlap/) |
| XenosWinTile | [gpu-window-tile-capture](lab/gpu-window-tile-capture/) |
| XenosWinAuto | [gpu-window-unarmed](lab/gpu-window-unarmed/) |

The [surface test](lab/gpu-reusable-surface/), [shared drawable test](lab/gpu-shared-drawable/),
[multi-client test](lab/gpu-multi-client/), [CGL discovery](lab/gpu-cgl-discovery/),
[CGL offscreen](lab/gpu-cgl-offscreen/), [CGL window](lab/gpu-cgl-window/),
and [WindowServer tests](lab/gpu-windowserver/) retain their source and checks.

## Rebuilding a profile

1. Fetch both forks and check out the driver tag in the USB inventory.
   Choose its build directory from the same row. For a plist-only or
   version-only retry, its package script records the predecessor archive
   and applies the change described in the source or metadata.
2. Use the [driver build dependencies](../BUILDING.md) and a checkout of
   [MacPPCKernelSDK](https://github.com/Goldfish64/MacPPCKernelSDK)
   at `12b9fac01a05c4ceeb27e42af53c76b810c135e7`. The historical
   recipes expect the cross toolchain at `gpu/lab/tx-build/toolchain/usr`
   and the SDK at `gpu/lab/MacPPCKernelSDK`. Link the SDK into each
   recipe's `source/MacPPCKernelSDK` when that recipe has a source tree.
   Most late recipes have a `compile-kext.sh` and a G5 `ld_classic` link
   script. Early link scripts take the prior build's PowerPC objects and
   the app source from the matching experiment.
3. Use your own Tiger 10.4.11 SDK or system copies of the linker reference
   files for the PowerPC tools. The CGL build expects the SDK under
   `gpu/lab/gpu-cgl-discovery/reference/tiger-root` and local copies of
   `crt1.o`, `bundle1.o` and `lib/libSystem.B.dylib` in that reference
   directory. The [reference fetcher](lab/gpu-cgl-discovery/fetch-reference.sh)
   can read the corresponding files from a Tiger machine. Use the
   `OSX360_TIGER_HOST`, `OSX360_G5_HOST`, and `OSX360_BUILD_HOST`
   environment variables for your SSH destinations. Older scripts may
   also take separate host and user settings or a local verified
   known-hosts file. Set and verify those locally before running them.
4. Build the OpenBIOS tag from the inventory. The
   [OpenBIOS profile guide](https://github.com/blub3/openbios/blob/gpu/quartz-extreme/Documentation/xbox360-gpu-profiles.md)
   gives the archive-name substitution for the derived ELFs. Run the
   matching `package*.py` using the predecessor and Tiger reference
   archives it specifies. The chain begins with the working Tiger mkext
   and two published driver changes, the Ethernet ring fix and native HDMI.
   Package scripts that verify an earlier mkext need that exact archive
   as input. Those generated artifacts and Apple's executable references
   are kept outside Git. Check the result against the full SHA-256
   manifest. Exact regeneration of all historical binary checksums has
   not been independently repeated from fresh checkouts.
5. Stage a separate XeLL entry after the Xbox handoff, preserve
   `kboot.conf.before-*.bak`, and verify the active profile and backup
   checksums before and after each activation. Hardware evidence belongs
   in the tracker and the commit message.

To refresh the USB inventory from a read-only copy, run
`python3 gpu/scripts/inventory-boot.py /path/to/OSX360BOOT-copy --date YYYY-MM-DD`
from the driver checkout. The observed source configuration in
[lab/observed-kboot.conf](lab/observed-kboot.conf) is a reference.

Fetch the fork before each session. Publish meaningful steps to the GPU
branches. A merge to main requires a passing hardware test and owner
approval, and uses a merge commit after integrating the latest main.
