# Handoff: GPU work continues on `main` (2026-11-25)

Written by Claude for the next GPU work (ChatGPT/Codex). As of 2026-11-25, `main` holds the platform work (SMP, audio, SMC) and the tested GPU drivers in one tree. Continue GPU work on a new branch from `main`, or merge `main` into your branch first.

**GPU work now follows [`docs/gpu-driver-plan.md`](gpu-driver-plan.md) on branch `gpu/xenos-driver`.** Claude and ChatGPT/Codex take turns on that branch, never at the same time. Start with its "Taking over" section and status table.

## What is in `main` now

| Merge | Content |
| --- | --- |
| `claude/audio` (earlier) | XenonAudio restarts its DMA channel on every engine start and reads the real play position: no repeated sound end, clean continuous playback. Interrupt mode and test switches off by default. `docs/audio-interrupt.md` |
| `claude/smp` (`401148a`) | SMP up to all six hardware threads, CPU stop/start, low-priority idle and waiting threads, the boot-spinner fix for CPUs started after login, the SMC lock fix, test scripts in `tools/host/`. **All off by default** (XenonPE personality keys). `docs/smp-design.md`, `docs/platform-log.md`, `docs/smp-driver-audit.md`. Two-hour test with six CPUs (`smp6c`): 115 heartbeats, none missed, 13 of 13 Ethernet checksum round trips, 0 failures. |
| `gpu/quartz-extreme` at `e663f26` (`6fee227`) | `XenonCoreGraphics` 0.0.5d48, `XenonFramebufferTest` 0.0.5d3, `include/XenosWindowAperture.h`, `include/XenosWindowServerFramebufferProbe.h`, `docs/gpu-*.md`, `docs/gpu-boot-checksums.txt`, `gpu/README.md`, `gpu/scripts/`, taken **unchanged**. `README.md` kept `main`'s version. |

**Not merged:** `gpu/lab/` (98.7% of the GPU branch's files: experiments, build recipes, staging scripts). It stays on `gpu/quartz-extreme`; links from `gpu/README.md` and `docs/gpu-*.md` into `gpu/lab/` point there. The branch itself was not changed.

**Source matches the tested GPU binaries.** Built from this tree with the standard Makefile, XenonCoreGraphics and XenonFramebufferTest have the same machine code as in the tested `XWOV.MKE` (`gpuwinoverlap`); only the kext header's build word and the anonymous-namespace symbol names differ.

**The repository `Info.plist` files are not what the tested profile uses.** The GPU packaging adds, at pack time:

- XenonFramebufferTest personality: `IOCFPlugInTypes` = `{ACCF0000-0000-0000-0000-000a2789904e = XenosWinOverlap.plugin}`, `IOGLBundleName` = `GLDriverXenosCompositorRetry`; libraries `com.apple.kpi.bsd`/`com.apple.kpi.mach` 8.11.0.
- XenonCoreGraphics `XenosDisplayAccelerator`: `IOGLBundleName` = `GLDriverXenosCompositorRetry`; libraries `kpi.bsd`/`kpi.mach` 8.11.0 instead of `com.apple.kernel.bsd` 1.1.
- XenonEthernet: version 0.0.6d1 in the profile, 0.0.5 in the repository (same code).

Decide whether these belong in the repository plists. Claude did not change GPU files.

## Boot stick (`OSX360BOOT`)

- **XeLL-Reloaded stores at most 64 `kboot.conf` entries and does not check.** 65 or more: XeLL shows only its own screen. Entry names and files must be FAT 8.3-safe.
- At the owner's request the stick was cut from 64 entries to a few. The 56 other GPU entries, their ELFs and mkexts went to the owner's PC: `stick-archive/2026-11-25-entries/` (137 files, MD5 manifest, `kboot.conf.before-cleanup`, restore steps in its README). GPU test folders, tools, `kboot-*.conf` and `kboot.conf.before-gpu*.bak` stayed on the stick. Scripts that expect the old entries will not find them.
- Current entries (12, `default=main6`): `main6`, `main1`, `audfix`, `smp6b`, `smp6c`, `park0`, `park1`, `pmapdef`, `gpuwinoverlap`, `openbios`, `framebuffer`, `hdminative`. `kboot.conf.before-main.bak` is the file before `main6`/`main1` were added (`default=smp6c`). `gpuwinoverlap` (`WINOVR.ELF` + `XWOV.MKE`) is unchanged.
- **`main6`** (`MAIN6.ELF` + `XMN6.MKE`) and **`main1`** (`MAIN1.ELF` + `XMN1.MKE`): all eight project kexts built from this `main`, with the tested plists above, plus IOGraphicsFamily 1.4.2 from `XWOV.MKE`. MD5: `MAIN6.ELF` `064982ed78001d0c8d50e3eb110af977`, `XMN6.MKE` `40f3051d32f6d0f54c23892f79980b30`, `MAIN1.ELF` `180c92db40c55181641153af5da55c53`, `XMN1.MKE` `0562a518aee9c5f206999ad2c271128e`. `main6` registers six CPUs and starts with one (`XenonSMPDeferStart`); `main1` is a plain single-CPU boot. Staged on 2026-11-25 and **not booted before the owner's next start**. If `main6` fails, set `default=smp6c` (tested) or `gpuwinoverlap` in `kboot.conf` (stick in any PC).
- Profile ELFs: `xbox360-profile-elf.py WINOVR.ELF XWOV.MKE <NEW.MKE> <NEW.ELF>` (`blub3/openbios`, branch `gpu/quartz-extreme`) points a copy of `WINOVR.ELF` at another mkext. `tools/host/repack_kext.py` replaces one kext in an mkext and proves the others are byte-identical.

## What SMP means for GPU code

- SMP is off unless the XenonPE personality has `XenonSMPThread1`..`5`. With `XenonSMPDeferStart`, the system boots on one CPU; start more with `sudo ./xenon-cpu-start <n>` in the Xbox home folder (`tools/smp`), stop with `... <n> stop`.
- **Device interrupts stay on CPU 0.** Secondary threads take only IPIs.
- **Idle CPUs answer interrupts up to 10 µs late** (`XenonSMPIdleLow`: the idle loop spins at low thread priority with interrupts off).
- **Any GPU path that uses "interrupts off" as a lock is broken with more than one CPU.** Known: `XenonPE::refreshFramebuffer` (`XenonPE_GPU.cpp`). See `docs/smp-driver-audit.md` and the SMP follow-up in `docs/gpu-milestones.md`. XenonCoreGraphics and XenonFramebufferTest ran under six CPUs in every SMP test (WindowServer up, desktop drawn), but no GPU test was run with SMP.
- A CPU started after login made Tiger take the screen back from WindowServer (boot spinner over the desktop). `XenonCPU::setProperties` now hands it back; GPU ownership code should not depend on the console state staying put.

## Rules learned on this hardware

- **Kernel imports:** never add a kernel symbol import that no kext in the booting mkext already imports; Tiger's offline link check accepts it but the boot fails. Resolve extra kernel functions at runtime (`resolveKernelSymbolExact` in XenonPlatform).
- **No boot-args, no NVRAM, no `panic.log`:** switch features with personality keys in the profile's mkext. A panic shows the grey screen and leaves nothing behind; log important state before risky steps.
- Shut the Xbox down over SSH (`sudo shutdown -h now`). Every restart lands in the Aurora dashboard and needs the owner to start XeLL by hand; restart only the GUI (`tools/host/xbox-gui.sh restart`) where possible.

## Build and test

- Build on the G5 (`ssh g5`): `make -C <Kext> OSX_VERSION=tiger DARLING_SHELL= AS=/usr/bin/as CC=$HOME/osx360-build/bin/g5-cc CXX=/usr/bin/powerpc-apple-darwin9-g++-4.2.1 LD=/usr/bin/ld_classic`, with `MacPPCKernelSDK` linked into the tree.
- Host test scripts: `tools/host/` (README there). Regression checks for every hardware run: SSH, WindowServer/Finder/Dock, a 10 MiB Ethernet round trip with checksums, audio playback.
- Results: `docs/platform-log.md` (platform), `docs/audio-interrupt.md` (audio), `docs/gpu-milestones.md` (GPU tracker, untouched by Claude).

## Open items

- GPU: milestone 7 of 8 (WindowServer acceleration), per `docs/gpu-milestones.md`; the SMP follow-up there.
- SMP: Tiger's scheduler does not know which CPUs share a core (three busy programs on six CPUs: two share a core); a single-CPU boot is about 8% faster per thread than an SMP boot.
- Audio: the first 0.5–1 s after silence is not heard (probably the TV muting on digital silence); interrupt mode and its two panics.
