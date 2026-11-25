# Xenos GPU driver plan

Plan for a complete Xenos graphics driver on Mac OS X 10.4.11 (Xbox 360 Slim), written 2026-11-25 from `main` at `9f0c40b`. Goal: Quartz Extreme, then OpenGL for applications, with as few reboots as possible.

The plan is shared: Claude and ChatGPT/Codex take turns on branch `gpu/xenos-driver`, never at the same time. The owner decides whose turn it is.

## Taking over

1. Fetch `blub3/osx360-drivers`, check out `gpu/xenos-driver`, and merge `main` into it if `main` has moved.
2. Read the status table and the newest log entry below, then `docs/handoff-gpu.md` (rules and boot stick) and `docs/platform-log.md` (platform state).
3. Continue at the first step that is not done. A step marked `in progress` was interrupted: its log entry says what is left.
4. Work on one step at a time. When it is finished or you stop:
   - update its row;
   - add a log entry (newest first) with the hardware result in numbers, or the reason it stopped;
   - commit and push.
5. Rules:
   - Define the pass criteria before a hardware run. Every run also checks SSH, WindowServer/Finder/Dock, Ethernet and audio.
   - Shut the Xbox down over SSH (`sudo shutdown -h now`). Every restart lands in the Aurora dashboard and needs the owner to start XeLL by hand; restart only the GUI (`tools/host/xbox-gui.sh restart`) where possible.
   - Back up `kboot.conf` before changing it and keep it at 64 entries or fewer.
   - Never enable a GPU interrupt source before vector `0x58` is routed (step 0.5): on thread 0 it panics today.
   - Never write all ones to `RBBM_SOFT_RESET` (`0x00F0`): it hangs the whole console without a panic screen. Reset only blocks whose bits are known.
   - Never add a kernel import that no booting kext already has. No 64-bit division in kernel code (the kernel exports no `__udivdi3`; `kextload -n -t` does not catch it).
   - Merge into `main` only after a passing hardware test and the owner's OK, with a merge commit named "Merge the tested …".

## Status

States: `open`, `in progress`, `done`, `failed`, `skipped`. "By" is Claude or ChatGPT. "Commit" is the commit that finished the step; a commit cannot name its own hash, so it writes "this commit" and the next person to edit the table fills in the hash (`git log -- docs/gpu-driver-plan.md`).

| Step | State | By | Commit | Result |
| --- | --- | --- | --- | --- |
| 0.1 Tiger GL stack and Quartz Extreme requirements | done | Claude | `ee56b82` | `docs/gpu-tiger-gl-stack.md`: Quartz Extreme needs ≥ 16 MB in the renderer record's video memory field; Xenos GLDs report 0 |
| 0.2 WindowServer restart over SSH | done | Claude | `443e1f3` | `tools/host/xbox-gui.sh restart`: desktop back in 8.5–9.1 s, 5 of 5; never use `killall loginwindow` |
| 0.3 Runtime kext loading, 64 MiB contiguous memory | done | Claude | `6808916` | Hand loading and unloading work; only 16 MiB contiguous; unused RAM `0x08100000`–`0x0FFFFFFF` (127 MiB) as heap, `docs/gpu-memory-map.md` |
| 0.4 Panic path, panic text, GPU interrupt line | done | Claude | `ec90e3e` | `docs/gpu-panic-and-interrupts.md`: hook `XenonPE::savePanicInfo`; GPU vector `0x58` is unrouted and panics on thread 0 |
| 0.5 Development boot profile `gpudev` (R4) | done | Claude | `542f003`, `06432f1` | Boots as stick default; heap, panic screen (owner read it) and vector `0x58` dispatch active; audio glitchy while the CPU refresh runs (fixed by step 1.1); `docs/gpu-dev-profile.md` |
| 0.6 Development loop script `tools/host/gpu-dev.sh` | done | Claude | `663e46a` | Build, import check, install, load, log, unload in about 13 s; `--key`, `--gui-down`, `--strict` tested |
| 1.1 Load, unload, display handback | in progress | Claude | `78abf83` | First session after boot works (4,127 frames, clean unload); a second session stalls at about frame 72; quiesce build installed, not run; see log |
| 1.2 Debug client, fence page, GPU interrupt | open | | | |
| 1.3 GPU memory manager | open | | | |
| 1.4 Command submission with address check | open | | | |
| 1.5 Hang recovery | open | | | |
| 1.6 Display: linear scanout or damage copy, flips, modes | open | | | |
| 2.1 `libxenos` | open | | | |
| 2.2 Earlier milestones as user-space tests | open | | | |
| 2.3 Host tools: PM4 decoder, shader tools, replay | open | | | |
| 2.4 EDRAM manager, 1080p with depth | open | | | |
| 2.5 2D operations | open | | | |
| 3.1 Copy and fill plug-in on `libxenos` | open | | | |
| 3.2 2D pass: pixel-exact and faster than software | open | | | |
| 4.1 GLD interface (logging GLD) | open | | | |
| 4.2 Hybrid GLD, Quartz Extreme switch | open | | | |
| 4.3 Compositor operations on the GPU | open | | | |
| 4.4 IOAccelSurface for app GL windows | open | | | |
| 4.5 Quartz Extreme pass (milestone 7) | open | | | |
| 5.1 Fixed-function shaders | open | | | |
| 5.2 Vertex data | open | | | |
| 5.3 Textures and framebuffer objects | open | | | |
| 5.4 Raster state, queries, readback | open | | | |
| 5.5 ARB programs, GLSL | open | | | |
| 5.6 Software fallback per draw call | open | | | |
| 5.7 Conformance, multiple contexts, Core Image | open | | | |
| 6.1 `XenosGPU` in the mkext, plists in the repository | open | | | |
| 6.2 Desktop smoke test (milestone 8) | open | | | |
| 6.3 Merge into `main` | open | | | |

## Log

Newest first.

- 2026-11-26 00:05, Claude: stopped for the day at the owner's request; the Xbox was shut down over SSH. **Where to continue:**
  1. Boot `gpudev` (the stick's default).
  2. `tools/host/gpu-dev.sh XenosGPU load --wait 20`, then `unload`. The installed build `78abf83` quiesces before halting the CP; check the "quiesce:" and "command processor halted, RBBM_STATUS" lines.
  3. Load and unload again; the 20 cycles are the pass criterion.

  If the second session still stalls at about frame 72, compare XenonCoreGraphics' start at boot with ours step by step. Every GPU stall needs a reboot (the owner has to start XeLL), so test only with the owner present. While `XenosGPU` is not loaded, `gpudev` redraws through the CPU, the desktop is slow and audio glitchy; `main6` is the profile for normal use.
- 2026-11-25/26, Claude: step 1.1, register comparison and two follow-up tests.
  - **Diagnostics:** `XenosGPU` dumps 109 GPU registers at every load (named a2xx CP, RBBM and MH registers, plus those XeLL and libxenon set; no data ports). A page-crossing check pads the ring with NOPs to dword `0x420` and checks a marker there.
  - **Session 0 vs session 1 after a fresh boot:** the page-crossing marker passed in both, and session 1 still stalled at frame 72 (now at `rptr=0x081F`, because of the padding). **So the stall is count-based (about 72 resolves), not tied to the ring position.**
  - **Register differences** at load, session 0 (XeLL's state) against session 1:
    - `CP_RB_BASE`/`CP_RB_CNTL` (`0x00020309`, with rptr write-back) and scratch write-back (`0x0770` = `0x20037`, `0x0774` = `0x01B7B000`), which were ours during session 0 too, so not the cause;
    - **`CP_ME_PS_EVENT_ADDR`/`DATA`** (`0x1818`/`0x181C`): `0x01B7C002`/`0x6D` from XeLL, 0 after a session;
    - `SQ_INST_STORE_MANAGMENT` (`0x3408`) `0x10800` against `0x180`;
    - status registers (`CP_CSQ_*_STAT`, `CP_MEQ_STAT`, `CP_STAT`).
  - **PS event test:** setting `CP_ME_PS_EVENT_ADDR` to a heap scratch word at start did not stick (0 again at unload, scratch untouched), and the next load found the GPU busy (`RBBM_STATUS` `0x80050110`: `GUI_ACTIVE`, `CP_NRT`, `MH`), so the start aborted in the 3D reset. Now opt-in only (`XenosGPUPsEvent`).
  - **New lead:** the stop halts the CP as soon as the ring is read (`rptr` = `wptr`), while the last resolve can still be running. Next build quiesces first (`CP_WAIT_FOR_IDLE`, a marker, then waits for `GUI_ACTIVE` to clear) and logs `RBBM_STATUS` after the halt.
- 2026-11-25, Claude: step 1.1, libxenon init registers.
  - **What I tried:** after a fresh boot, `XenosGPU` wrote libxenon's `Xe_pSetup`/`Xe_pResetCP`/`Xe_pMasterInit` registers on every start.
  - **Result:** session 0 passed (1,447 frames in 20 s); session 1 stalled after 71 frames again (`rptr=0x0410`). **These registers are not the cause.**
  - **What XeLL leaves at boot** (read before writing): `0x3214` = 3, `0x3294` = 1, `0x3408` = `0x10800`, `0x07D4` = 0, `0x2054` = `0x2154` = `0xFF00`, `0x3C10` = 0, `0x3C40` = `0x1A`, `0x3C4C` = 0, `0x87E4` = 0. XeLL does not use libxenon's `Xe_Init` values. `0x3408` is `SQ_INST_STORE_MANAGMENT`, which `XenosGPU`'s display state sets to `0x180`.
  - **Change:** the writes are now opt-in (`XenosGPULibxenonInit`).
  - **Clear so far:** the first session after boot always works (4,127 and 1,447 frames). Every session after a `XenosGPU` stop and start stalls at frame 71.
  - **Next:** dump a set of GPU registers that is safe to read (no FIFO or data-port registers) at the start of session 0 and session 1, and compare them, instead of guessing.
- 2026-11-25, Claude: step 1.1, build `560c59e` (3D block reset only on a busy pipeline), after a fresh boot.
  - **First session: pass.** The reset was skipped (`RBBM_STATUS` `0x00040110`), both markers passed, **4,127 frames in 62 s** (about 66 per second) with no busy tick, and the unload drained the ring, halted the CP and restarted the CPU refresh. **The libxenon 3D reset without libxenon's `Xe_pInit1` register set was what made every run since 23:01 stall at frame 73.**
  - **Second session (cycle 1 of 20): fail.** It stalled after 71 frames (`rptr=0x0410`, `RBBM_STATUS=0xF3250110`), although the pipeline was idle at start and no 3D reset ran. This matches the first build's reload failure at 22:48. So after one complete session, the next stalls at about frame 71.
  - **Next candidate:** libxenon's `Xe_pSetup`, `Xe_pResetCP` and `Xe_pMasterInit`, which XeLL runs at boot, write registers neither XenonCoreGraphics nor `XenosGPU` writes:
    - around the CP: `0x07D0` = `0xFFFF`, `0x07F0` = 0, `0x0774` = 0, `0x0770` = 0, `0x3214` = 7, `0x3294` = 1, `0x3408` = `0x800`, `0x07EC` = `0x1A`;
    - after the CP start: `0x07D4` = 0 then 1, `0x2054` = `0x1E`, `0x2154` = `0x1E`, the EDRAM `0x3C10` = `0xD`, `0x3C40` = `0x17` with `0x3C48`/`0x3C4C`, then `0x3C40` = `0x1017` and `0x87E4` = `0x17`.

    If the CP reset clears some of them, a first session still has XeLL's values and a second does not. Next build: the same writes on every start.
- 2026-11-25, Claude: step 1.1, GUI restart test.
  - **Result:** fresh boot, `xbox-gui.sh restart` (desktop after 20.8 s), then `XenosGPU` for 60 s: stall after 73 frames again (`rptr=0x0428`). The GUI restart is not the difference.
  - **New lead:** the only long run (22:47) was the first build, which reset only the CP. Every build since 23:01 resets the 3D blocks at start (libxenon values) **without** libxenon's following `Xe_pInit1` register set (`0x01A8` = 0, `0x0E6C` = `0xC0F0000`, `0x3400` = `0x40401` then `0x40400`, `0x3300` = `0x3A22`, `0x340C` = `0x1003F1F`, `0x00F4` = `0x1E`). Every one of those runs stalled at frame 71–73, even on a fresh boot.
  - **Next build:** the 3D reset runs only when `RBBM_STATUS` shows `GUI_ACTIVE` at start, so an idle GPU starts exactly as in the 22:47 run. If that runs long, the recovery path must add the `Xe_pInit1` registers after the reset.
  - **Still unexplained:** the reload failure of the first build (cycle 1, 71 frames).
- 2026-11-25, Claude: step 1.1, two more tests (`520626e`), each after a shutdown and restart by the owner.
  - **Ring in the GPU heap:** `XenosGPU` takes its ring, marker and vertex buffers from the heap at `0x08200000` (uncached; `XenosGPUTigerMemory` selects the old Tiger allocations). Both markers passed, then the GPU **stalled after 73 frames again**, at the same `rptr=0x0428` and `RBBM_STATUS=0xEAA50110`. The ring's physical address is not the cause.
  - **50 ms presentation** (`XenosGPUSlowPresent`): stall after 71 frames (about 3.5 s, `rptr=0x0410`, the resolve of frame 71). **The stall is count-based**, about 71–73 frames at any rate.
  - **Interrupts are not involved:** `CP_INT_CNTL` and `RBBM_INT_CNTL` were 0 at start and at the stall, and the only pending source was `RBBM_INT_STATUS` `0x00080000` (GUI idle) with `MASTER_INT_SIGNAL` 1.
  - **The one success** (2,089 frames at 22:47) came 16 minutes after boot and 6 minutes after a GUI restart (`xbox-gui.sh` at 22:41). All failures were on a fresh boot without a GUI restart, or a reload right after a session. XenonCoreGraphics, with the same per-frame commands, runs for hours from boot.
  - **Next experiment:** fresh boot, `xbox-gui.sh restart`, then `XenosGPU` for 60 s.
  - **Other candidates:** differences between XenonCoreGraphics' start at boot and a runtime start: the 1 s delay before its first frame, the `IOCFPlugInTypes` plug-in and the GLD that are absent on `gpudev`, and the display state WindowServer sets at its first start.
- 2026-11-25, Claude: step 1.1, after the owner's power cycle (`gpudev` booted at 23:00).
  - **libxenon 3D reset:** `XenosGPU` now resets the 3D blocks the way libxenon's `Xe_pReset` does (`RBBM_SOFT_RESET` = `0x8064`, then 0, then `0x11800`, then 0, repeated until `RBBM_STATUS` bit 31 is clear, at most 100 rounds), then resets the CP as before. It does not hang the console.
  - **Load on a fresh, idle GPU** (`RBBM_STATUS` `0x00040110`): the reset finished in 1 round, `0x00E0` read `0x10`, both markers passed. The GPU **stalled after 73 frames** (about 1.1 s; ring not drained at unload).
  - **Next load on the stalled GPU:** the reset cleared some bits (`0xEAA50110` → `0xE0250110`) but not `GUI_ACTIVE`, `RB_CNTX`, `MH` or `CP_NRT`, even after 100 rounds on each of 3 attempts. **A stalled GPU is not recoverable by software we know of; only a reboot clears it.**
  - **Where it stalls:** `rptr=0x0428`, `wptr=0x042A`, inside the resolve of frame 73, just past the ring's first 4 KiB (`0x400` dwords). The earlier failure stopped at about `0x412`. ChatGPT's tracker also records a ring stall at `0x03DF`.
  - **Leading hypothesis:** XenonCoreGraphics, which ran stably for hours, always gets its ring at `0x01780000` (Tiger's bank 0, below 32 MiB; `system.log` of every `main6` boot). `XenosGPU`, loaded at runtime, got rings at `0x1AA78000`–`0x1BA38000` (bank 1). OpenBIOS limits bank 0 with the comment "This reduces freezes, but they still may occur".
  - **Next experiment:** put the ring and vertex buffers into the GPU heap at `0x08200000` (low memory outside Tiger), on a fresh boot. Alternatively, present every 50 ms instead of 15 ms, to tell a time-based stall from a ring-position one.
  - **Also new:** a stall log line, with ring pointers, `RBBM_STATUS`, `CP_ME_STATUS` and `CP_STAT` after 150 ms without progress.
- 2026-11-25, Claude: step 1.1, full reset test: **console hang.**
  - **What I tried:** `XenosGPU` changed to reset every RBBM block at start and stop: halt the CP, `RBBM_SOFT_RESET` (`0x00F0`) = `0xFFFFFFFF`, `IOSleep(30)`, then 0, logging `RBBM_STATUS` and the display surface before and after.
  - **Result:** it was loaded once, on the GPU still hung from the earlier cycles. The Xbox stopped answering during the load, before any log line reached the host: no ping, "No route to host". The owner saw a frozen or black screen, not the panic screen, so it was a hard hang (a CPU access to the GPU or bus that never completes), not a kernel panic. The Xbox needed a power cycle and XeLL.
  - **Code:** that version is not committed; the branch keeps the CP-only reset of `da34871`.
  - **New rule:** never write all ones to `RBBM_SOFT_RESET`.
  - **Next:** find the Xenos bit layout of `RBBM_SOFT_RESET`. Then reset only the blocks `RBBM_STATUS` shows busy (`RB`, `SQ`, `PA`, `SC`, `SX`, `VGT`), not the memory hub or bus interface, and try it first on a freshly booted, idle GPU.
- 2026-11-25, Claude: step 1.1 in progress, stopped here.
  - **The driver:** `XenosGPU` (`da34871`, hand-loaded, `--strict` imports pass) uses XenonCoreGraphics' ring start-up, firmware load and display state. It stops XenonPE's CPU refresh and presents the desktop with the GPU every 15 ms.
  - **First load: pass.** Both `MEM_WRITE` markers passed, the CP started on attempt 1, 2,089 frames in 32 s (about 65 per second) with no busy tick. At unload the ring was drained, the CP halted and the CPU refresh restarted. Idle CPU was 88% free, against 72% with the CPU refresh.
  - **Second load: fail.** The GPU stopped completing frames after 71 frames (about 1 s; 170 ticks waited, ring not drained at unload).
  - **Every load after that:** the micro-engine init and the first marker pass, but the marker after the display state never arrives, on all 3 attempts. `RBBM_STATUS` stays busy, for example `0xF3250110`: `GUI_ACTIVE`, `RB`, `SQ`, `PA`, `SC`, `SX`, `CP_NRT` and `MH_BUSY`. 0 of 20 cycles passed.
  - **Diagnosis:** the start-up reset (`RBBM_SOFT_RESET` = 1) resets only the CP. The 3D blocks stay hung, so the display state's `CP_WAIT_REG_EQ` on `RBBM_STATUS` never completes. XenonCoreGraphics only ever started once per boot.
  - **State:** no panic; the desktop stays up on the CPU refresh.
  - **Next:** a full soft reset (kgsl does `RBBM_SOFT_RESET` = `0xFFFFFFFF`, 30 ms, then 0 on Adreno 2xx), with `RBBM_STATUS` logged before and after and the display surface address rewritten afterwards. Then also a full reset at unload, and the 20 cycles again. Whether the full reset touches the display is not known yet.
- 2026-11-25, Claude: step 0.6 done; phase 0 is complete.
  - **The script:** `tools/host/gpu-dev.sh KEXT [build|install|load|unload|cycle|log]`.
    - It syncs the tree to the G5, builds and checks the kernel imports.
    - Any `___` compiler helper fails the check: the Tiger kernel exports none of `__udivdi3`, `__divdi3`, `__umoddi3`, the 64-bit shifts or the float conversions.
    - Imports outside `tools/gpu/main6-boot-imports.txt` (1,419 symbols, the union of the `main6` boot kexts) are listed, and are fatal with `--strict`.
    - It installs in `/Library/OSX360Dev`, can set personality keys for one load (`--key`, the plist is restored afterwards), loads, waits, unloads (with `--gui-down` around the unload), and shows the kext's kernel log lines since that load.
  - **Tested with `XenosGPUProbe` on `gpudev`:**
    - full cycle in 12.8 s;
    - `build --strict` exits 1 on the probe's three non-`main6` imports;
    - a cycle with `--key XenosGPUProbeStopRefresh --gui-down` paused and resumed the refresh, restored the plist, and brought the desktop back (19.8 s on `gpudev`).
  - **Also seen:** on `gpudev` only XenonFramebufferTest is attached to the GPU next to the probe.
  - **Next:** step 1.1, `XenosGPU` load and unload with GPU presentation.
- 2026-11-25, Claude: `gpudev` audio.
  - **Result:** the owner heard the boot check glitchy. A `say` with XenonPE's CPU refresh paused (new probe switch `XenosGPUProbeStopRefresh`) was clean; one with it running was glitchy.
  - **Cause:** `refreshFramebuffer` keeps interrupts off on CPU 0 while it converts the whole screen, every 500 ms.
  - **Fix:** step 1.1 now requires `XenosGPU` to present the desktop with the GPU and stop the CPU refresh, with a clean `say` as a pass criterion. Fallback: make `refreshFramebuffer` enable interrupts between rows with the next XenonPlatform change that needs a boot.
- 2026-11-25, Claude: step 0.5 done. `gpudev` booted at 22:31 as the stick's default.
  - **Checks:**
    - seven Xenon kexts, 6 CPUs registered and 1 running, desktop up (very slow, as expected), no panic;
    - XenonPE logged the heap, the armed panic screen (with the kernel's font) and the `0x58` dispatch;
    - 10 MiB Ethernet round trip OK;
    - the panic screen test drew readable, correctly oriented text, confirmed by the owner, and the desktop came back.
  - **Costs:** the CPU refresh takes about 24% system time when idle and slows SSH transfers. Phase 1 removes it.
  - **RAM:** RAM does not survive a shutdown (the probe's pattern was gone).
  - **Still open:** the audio check by ear; the deliberate panic test (optional).
  - **Next:** step 0.6, the development loop script.
- 2026-11-25, Claude: `gpudev` staged as the stick's default (13 entries, backup `kboot.conf.before-gpudev.bak` with `default=main6`; only the four expected files changed). The Xbox was shut down over SSH at the owner's request; the owner boots it. Next: the boot test in `docs/gpu-dev-profile.md`. If `gpudev` does not come up, set `default=main6` on the stick from a PC.
- 2026-11-25, Claude: step 0.5 in progress: `gpudev` is built, not staged and not booted. Details, checksums and the boot test in `docs/gpu-dev-profile.md`.
  - **XenonPlatform**, three features, each off unless its personality key is set:
    - `XenonPanicScreen`: the panic text on screen with the kernel's own font, plus a record at `0x08100000`; test functions without a panic.
    - `XenonGPUInterrupt`: dispatch of vector `0x58` instead of the panic, and silencing at the GPU if nobody handles it.
    - `XenonGPUHeap`: publishes 126 MiB at `0x08200000`.
  - **Imports:** the same kernel imports as the tested XenonPlatform. A first version imported `IODelay` and `IOSleep`; both were removed. Tiger's offline link check accepts it.
  - **Packing:** `XGDV.MKE` = `XMN6.MKE` without XenonCoreGraphics, with XenonFramebufferTest's plist minus `IOGLBundleName` and `IOCFPlugInTypes`, and the new XenonPlatform; the other six kexts are byte-identical. It was made with the new `tools/host/mkext_edit.py`. `GPUDEV.ELF` differs from `WINOVR.ELF` only in the mkext name.
  - **Probe:** `XenosGPUProbe` got the two test switches, and its RAM test moved to the heap range. Its pattern written at 22:04:45 was fully intact at 22:25:12.
  - **Next:** stage on the stick and boot with the owner present.
- 2026-11-25, Claude: step 0.4 done (analysis only, no hardware change), details in `docs/gpu-panic-and-interrupts.md`.
  - **Panic path in the 10.4.11 kernel:** `Debugger` packs the panic text (up to 2,040 bytes) and calls `PESavePanicInfo`, which calls the platform expert's virtual `savePanicInfo`. In XenonPE (an `IODTPlatformExpert`) that ends in NVRAM code that returns 0, so the text is lost today.
  - **If a hook returns a length:** the kernel stops the other CPUs, draws the panic dialog, and hangs in `IOPlatformExpert::haltRestart(kPEHangCPU)` (`b .`), with no reboot.
  - **Plan:** `XenonPE::savePanicInfo` draws the text with a built-in public-domain font into both framebuffers, for a photo, and copies it to the unused RAM at `0x08100000`.
  - **GPU interrupt:** CPU vector `0x58`, not routed through the PCI bridge. `XenonInterruptController` panics on purpose if thread 0 takes it. The GPU already raises it on another thread, as seen in SMP stage 1a. `CP_INT_CNTL` provides ring, software and error interrupts.
  - **New rule:** no GPU interrupt source may be enabled before the `gpudev` boot routes `0x58`.
- 2026-11-25, Claude: correction from the owner. After every restart the Xbox starts the Aurora dashboard, and the owner has to start XeLL by hand. The reboots during step 0.2 ("no owner needed") came back only because the owner did that. A reboot (R3) is therefore never unattended; only R0–R2 are. The restart cost table, the rules, step 0.4 (panic text instead of an automatic reboot) and the risks are updated. The owner heard the step 0.3 `say` check clearly.
- 2026-11-25, Claude: step 0.3 done, details in `docs/gpu-memory-map.md`.
  - **Probe:** `XenosGPUProbe` (new, hand-loaded from `/Library/OSX360Dev`, not in the package) matches the Xenos PCI device in its own category.
  - **Runtime loading works:** `kextload` succeeded with the desktop up, and the probe attached next to XenonFramebufferTest, XenosController and XenosDisplayAccelerator. `kextunload` succeeded with the desktop up and with the GUI down (`xbox-gui.sh stop`), and it loaded again afterwards. The first build failed to load on `___udivdi3` even though `kextload -n -t` accepted it, so 64-bit division in kernel code is now in the rules.
  - **Contiguous memory:** 16 MiB passed on all five loads, 32, 64 and 128 MiB always failed. Tiger has only 272 MiB of RAM (OpenBIOS gives it `0x00000000`–`0x01FFFFFF` and `0x10000000`–`0x1EFFFFFF`), with 37–53 MiB free.
  - **Unused RAM:** the device tree marks `0x08100000`–`0x0FFFFFFF` (127 MiB) available, and Tiger does not manage it. A pattern written to every 256th byte (520,192 words, 88 ms) read back with 0 mismatches, again at unload 2.5 minutes later (across a WindowServer restart and an Ethernet transfer), and was fully intact at the next load. This is the planned GPU heap, without a firmware change. Retention over hours is checked by loading the probe again later in the same boot (it reports the intact words).
  - **Memory-pressure step skipped:** 32 MiB already fails on a fresh boot, and the unused range makes large allocations unnecessary.
  - **Regression checks:** 10 MiB Ethernet round trip OK, 6 CPUs registered and 1 running, eight Xenon kexts loaded, desktop up, no panic, `say` returned 0.
- 2026-11-25, Claude: step 0.2 done.
  - **Method:** `tools/host/xbox-gui.sh` with `stop`, `start` and `restart`.
    - `stop` switches the loginwindow console line in `/etc/ttys` off and sends `HUP` to launchd. launchd ends loginwindow, WindowServer exits, and nothing respawns.
    - `start` starts WindowServer `-daemon` by hand (it waits until loginwindow connects, then continues under a new PID), restores `/etc/ttys` and sends `HUP` again. loginwindow starts once and autologin brings the desktop back.
  - **Results:**
    - From a clean boot, 5 restarts in a row passed. GUI down after 0.9–1.1 s, Finder and Dock back after 8.5–9.1 s, new WindowServer, loginwindow, Finder and Dock PIDs each time, one loginwindow start per restart, no abort.
    - `stop`, 30 s with no WindowServer or loginwindow, then `start`: desktop after 8.4 s. Usable for unloading kexts in step 0.3.
    - Each new WindowServer loaded `XenosWinOverlap.plugin` again ("XenosWinOverlap start" in `system.log`), so the plug-in named in `IOCFPlugInTypes` is read at every WindowServer start. Whether a changed property is picked up can only be tested once a driver sets it (step 1.1). Re-reading `IOGLBundleName` is checked in step 4.2, because WindowServer loads no GLD today.
  - **Never `killall loginwindow`:** it races with launchd's respawn and was not repeatable. After a hand-started WindowServer has been ended that way, the service name is dead, loginwindow aborts in a loop and only a reboot helps. That happened once more during this step and was recovered with `sudo reboot` (desktop after 126 s).
  - **Regression checks afterwards:** 10 MiB Ethernet round trip matched its checksums, 6 CPUs registered and 1 running, no panic, and the owner heard the `say` check clearly.
- 2026-11-25, Claude: step 0.2 in progress, stopped here.
  - **How Tiger runs the GUI:** WindowServer runs as user `windowserver`, child of `launchd`, and is a mach_init on-demand service (`com.apple.windowserver`, `/etc/mach_init.d/WindowServer.plist`). loginwindow comes from the console line in `/etc/ttys`. Autologin is on (user 501).
  - **`sudo killall loginwindow`:** WindowServer exits within 1 s. The new loginwindow (root) waits forever in `CGSServerPort` → `_CGSGetCoreGraphicsServerVersion` (`sample`): the on-demand restart does not happen.
  - **WindowServer started by hand:** `sudo nohup …/WindowServer -daemon &` brought the desktop back in 6.2 s. The new WindowServer loaded `XenosWinOverlap.plugin` again and completed its startup fences, so plug-ins are re-read at each WindowServer start.
  - **Not repeatable:** the same sequence in `tools/host/xbox-gui.sh restart` worked once (7.7 s) and failed on the next run. loginwindow then aborts in a loop (`Abort trap`, launchd pauses 30 s), and a hand-started WindowServer came up and exited again without a crash report before any loginwindow connected. Starting it through `/usr/libexec/StartupItemContext` failed on the first run: no WindowServer process.
  - **Recovery:** each failure was recovered with `sudo reboot` over SSH, desktop back in 155 s and 112 s. (Correction: the owner started XeLL from the Aurora dashboard after each reboot; a reboot is never unattended.) Afterwards: 10 MiB Ethernet round trip OK, 6 CPUs registered and 1 running, no panic, and the owner heard the `say` check clearly.
  - **Also seen:** in a normal session WindowServer has no GLEngine or GLD loaded (`vmmap`), only `XenosWinOverlap`.
  - **Next ideas:**
    - re-register the on-demand service after WindowServer exits (`/usr/libexec/register_mach_bootstrap_servers /etc/mach_init.d`, in the system bootstrap context) and let loginwindow start WindowServer itself;
    - or stop the GUI with the `/etc/ttys` console line off before ending WindowServer.
  - **Fallback:** if no reliable WindowServer restart is found, WindowServer-side changes use a reboot over SSH (R3, about 2 min, with the owner starting XeLL), still cheaper than a stick change.
- 2026-11-25, Claude: step 0.1 done, read from the installed Tiger binaries (no hardware change). Results in `docs/gpu-tiger-gl-stack.md`. Main finding: CoreGraphics enables the GL compositor only if the first renderer record reports at least `GLCompositorMinimumVRAM` (16 MB) at `+0x30`. Every Xenos GLD so far copies Apple's software renderer record, which reports 0. The AGP requirement (`GLCompositorRequiredClasses` = `IOAGPDevice`) is bypassed by the non-zero `AccelCaps` XenonCoreGraphics already sets. All 63 GLD functions and the 56-byte renderer record layout are listed. Tools: `tools/gpu/macho.py`, `tools/gpu/annotate-ppc.py`. Also on that boot: `main6` booted for the first time (6 CPUs registered, 1 running, WindowServer/Finder/Dock up, all eight kexts loaded, no panic), a 10 MiB Ethernet round trip matched its checksums both ways, and the owner heard the `say` audio check clearly. Next: step 0.2. The smallest Quartz Extreme test, a GLD that changes only the VRAM field, needs only the WindowServer restart from 0.2.
- 2026-11-25, Claude: plan written from `main` at `9f0c40b`. No step started. The Xbox is shut down; its stick boots `main6` (not yet booted). Phase 0 needs the Xbox on.

## Where the GPU work stands

What works on hardware (details: `docs/gpu-milestones.md`):

- Command ring, fences, clears, triangles, textures and their filtering, blending, depth, stencil, scissor, render-to-texture, VBlank flips.
- User clients with several Tiger processes, CGL and AGL rendering in a test process, Tiger finding the Xenos renderer and its GLD.
- A WindowServer copy-region plug-in that moves one window area through the GPU.
- The desktop: Tiger draws into a linear 8 MiB framebuffer. Every 15 ms `XenosController::handleTimer` draws the whole framebuffer as a texture and resolves it into the tiled scanout buffer. Without XenonCoreGraphics, `XenonPE::refreshFramebuffer` does the same conversion on the CPU every 500 ms.

Why it is slow to develop, and why Quartz Extreme is still off:

- **Every test lives in the kernel.** XenonCoreGraphics has 28 user client selectors (`XenosQuadProtocol.h`), one for each test: quad, blended batch, stencil batch, mipmap batch and so on. Each one renders into a fixed 32 × 32 target and checks its own pixels in the kernel. Every new test therefore needed a new kext, a new mkext, a new boot entry and a cold boot: **56 GPU boot entries** were on the stick before the cleanup. Several of those boots failed (unresolved `proc_selfpid`, a ring stall at startup, a WindowServer crash from an empty plug-in slot), and some needed the owner at the console.
- **No generic path.** A user program cannot send its own commands or allocate GPU memory. The GLD can therefore only call the test selectors.
- **The GLD reports a software renderer** (`accelerated=0`), so WindowServer never turns Quartz Extreme on. The GPU window copy takes about 1.1 s for 7,252 pixels in 32 × 32 tiles, with the CPU writing the result to the screen, which is slower than software.
- No GPU interrupts (everything is polled by timers), no GPU hang recovery, and no memory manager.

The plan below replaces the test selectors with a small, generic kernel driver and moves all rendering logic to user space, where a change needs no reboot.

## Restart cost

| Kind | What it costs | When it is needed |
| --- | --- | --- |
| R0 | Nothing: run a program over SSH | User-space library, tests, GLD in a test process |
| R1 | WindowServer restart, about 9 s (`tools/host/xbox-gui.sh restart`), no owner needed | GLD or plug-in used by WindowServer changes |
| R2 | `kextunload` + `kextload`, seconds | Kernel driver loaded by hand (not in the mkext) |
| R3 | `sudo reboot`, about 2–3 min; **the owner must start XeLL from the Aurora dashboard** | Boot-resident code changes, or recovery after a crash |
| R4 | Stick change + reboot | OpenBIOS or mkext changes |
| R5 | Owner power-cycles the Xbox and starts XeLL from Aurora | Hang or panic (grey screen) |

**Rules that keep restarts rare:**

- During development, the new GPU kernel driver is **not in the mkext**. It is loaded by hand from a folder outside `/System/Library/Extensions`, so Tiger never loads it at boot. A bad build can at most crash the running system, and the next boot is clean.
- Every kernel feature is generic. Tests, shaders and rendering logic are user-space programs. The kernel checks no pixels.
- Registry properties that WindowServer reads (`IOGLBundleName`, `IOCFPlugInTypes`, `IOAccelTypes`) are set by the running driver, not by personalities packed into the mkext.
- **Every reboot needs the owner:** after any restart the Xbox comes up in the Aurora dashboard, and the owner has to start XeLL by hand. Only R0–R2 run without the owner, so development runs there. Crashes are avoided rather than recovered: the address check stops bad command buffers before they reach the GPU, the driver recovers GPU hangs itself, and a panic should at least leave its text (step 0.4).
- Test on the host where possible: command-stream and shader decoding, and shader assembly, run on the PC.

**Expected boot-stick changes:** 1–2 in phase 0, none in phases 1–5, 1–2 in phase 6.

## Target design

Kernel driver `XenosGPU` (new bundle ID; it replaces XenonCoreGraphics in phase 6):

- **GPU memory:** a contiguous heap (64–128 MiB; candidate: the 127 MiB at `0x08100000`–`0x0FFFFFFF` that Tiger does not manage, `docs/gpu-memory-map.md`), allocations as handles and owned by a client, mapped cacheable into the client's task with explicit cache flushes, and freed when the client exits. No cache-inhibited user mappings: Tiger's `memcpy` and `bzero` use `dcbz`.
- **Command submission:** clients build PM4 command buffers in their own GPU memory. The kernel ring holds only `CP_INDIRECT_BUFFER` packets and fence writes. The kernel checks that every address in a buffer belongs to the client. A root-only development mode skips the check.
- **Fences:** a timestamp written by the GPU into a page mapped read-only into every client, and a wait call that uses the GPU interrupt.
- **Hang recovery:** a watchdog on ring progress, a soft reset (`RBBM_SOFT_RESET`) with ring and firmware reinit, an error returned to the client, and the display back on the software path.
- **Display:** scanout address and format, page flip at VBlank, 720p/1080p switching together with XenonFramebufferTest, and handing the display back to XenonPE on unload (`kXenonFuncPlatformStartFB`, which already exists).
- **IOAccelerator:** the surface client WindowServer uses (`IOAccelSurfaceConnect.h`: create, shape, lock, read, flush), plus a private client for the GLD and the 2D plug-in.
- **Debug client (root only):** reads and writes registers and GPU memory, so most hardware questions are answered by a program, not a new kext.
- **SMP:** all state behind one lock or command gate. Interrupts arrive on CPU 0. Test with six CPUs.

User space:

- `libxenos`: GPU memory, a PM4 command builder, state blocks, loading shader binaries, fences, an EDRAM manager. The tests, the GLD and the 2D plug-in all use it.
- **GLD bundle:** starts as a wrapper around Apple's software renderer (`GLDriver.bundle`; `gpu/lab/gpu-compositor-retry` already forwards every call to it) and takes over operations one at a time.
- **2D plug-in:** the WindowServer copy and fill plug-in, rebuilt on `libxenos`.

Host tools (`tools/gpu/`):

- a PM4 decoder built from the register headers already in the tree (`a2xx.xml.h`, `adreno_pm4.xml.h`);
- a shader assembler and disassembler built on `instr-a2xx.h`;
- a record-and-replay format for command streams captured on Tiger.

## Phase 0: check the unknowns, build the development boot

All checks are R0 over SSH unless marked.

1. **Tiger's GL stack:**
   - List the GLDs installed on the Xbox; the ATI R300 GLD is a reference for a similar GPU.
   - List the exports of GLEngine and the GLDs.
   - Find what WindowServer checks before enabling Quartz Extreme: accelerated flag, video memory size, texture rectangle, `AccelCaps`.

   Result: a short requirements note.
2. **Restart WindowServer without rebooting.** Find a reliable way over SSH, for example `killall loginwindow`, or keeping the GUI down through the console line in `/etc/ttys`. Check that WindowServer reads `IOGLBundleName`, `IOCFPlugInTypes` and `IOAccelTypes` again when it starts. Time one restart.
3. **Runtime loading:**
   - Load a probe kext that matches the Xenos PCI device while WindowServer runs.
   - Unload it with the GUI down.
   - Allocate 64 MiB of physically contiguous memory after a long uptime.

   If the allocation fails, OpenBIOS reserves the heap instead (that is then part of the boot change in step 5).
4. **Panic and hang path:**
   - Find in the 10.4.11 kernel which platform call a panic ends in. XenonPE overrides no halt or restart function today.
   - Plan a way to keep the panic text: on screen instead of the grey panic screen, or sent over Ethernet. A RAM area kept across a reboot probably does not survive the Aurora dashboard, and every reboot needs the owner to start XeLL anyway.
   - Find the Xenos interrupt line. Free60 Linux and the interrupt controller in `XenonPlatform/src/Interrupts` are the sources.
5. **Development boot profile `gpudev` (R4, once).** The `main6` kexts without XenonCoreGraphics, plus the platform changes from steps 3 and 4: the `XenonPE::savePanicInfo` panic-text hook and the routing of GPU vector `0x58` (`docs/gpu-panic-and-interrupts.md`). Step 0.3 found the heap in RAM Tiger does not manage; the firmware should mark it reserved in the device tree, so it is not claimed by anything else. The desktop then runs on XenonPE's CPU refresh until `XenosGPU` is loaded. `main6` stays the stick default until `gpudev` passes the usual checks (SSH, desktop, Ethernet, audio).
6. **Development loop:**
   - sync the source to the G5 and build there;
   - copy the kext to the Xbox and load it;
   - run the tests;
   - collect `system.log`.

   One script, `tools/host/gpu-dev.sh`, does all of it, including the import check against the booting kexts (the import rule applies from the first build, so phase 6 has no surprises).

## Phase 1: kernel core (R2 for each change)

1. Load and unload: take the GPU from XenonPE, bring the ring up with the checked reset from 0.0.5d46, present the desktop with the GPU (stop XenonPE's CPU refresh with `PlatformStopFB`), and give the display back on unload (`PlatformStartFB`). Pass: 20 load and unload cycles with the desktop up, and a clean `say` while `XenosGPU` is loaded (on `gpudev` the CPU refresh makes audio glitchy).
2. Debug client, fence page and the GPU interrupt. Pass: a fence wait by interrupt, plus a VBlank count that matches 60 Hz.
3. GPU memory manager. Pass: allocate, map, free and exit with memory still held, without leaks (heap counters back at their start values).
4. Command submission with the address check, and the FIFO queue across clients. Pass: the existing clear, triangle and texture tests as user programs, run from several processes at once, fences in order.
5. Hang recovery. Pass: a deliberately bad command buffer resets the GPU, the client gets an error, the desktop keeps running, and the next submission works.
6. Display: check whether the display controller can scan out a linear surface (debug client, no rebuild).
   - If it can, the desktop needs no copy at all.
   - If not, copy only the changed areas instead of the whole screen every 15 ms.

   Then page flips at VBlank and 720p/1080p switching. Pass: lower CPU and GPU load on an idle desktop, both modes work.

## Phase 2: user-space library and tests (R0)

1. `libxenos`.
2. Port every earlier GPU milestone to a user-space test: clears, triangles, textures, filtering, blending, depth, stencil, scissor, render-to-texture, flips. All of them in one regression run of a few minutes, instead of one cold boot each.
3. Host tools: PM4 decoder, shader assembler and disassembler, capture and replay.
4. EDRAM manager: full-screen 1080p render targets with depth, through tiling (10 MiB EDRAM; 1080p colour alone is about 8 MiB).
5. 2D operations: fill, copy, blit with format conversion, alpha compositing.

## Phase 3: 2D acceleration in WindowServer (R1)

1. Rebuild the copy and fill plug-in on `libxenos`. Copies and fills run entirely on the GPU in the framebuffer: no 32 × 32 tiles, no CPU copy of the result. Implement every plug-in slot (the beam-position slot crashed WindowServer before). Set `IOCFPlugInTypes` from the driver at runtime.
2. Pass: window moves and fills match the software result pixel for pixel, and are faster than software. Measure it.

This gives a faster desktop even before Quartz Extreme, and is the fallback if phase 4 takes long.

## Phase 4: Quartz Extreme (R0 for test programs, R1 for WindowServer; milestone 7)

1. **GLD interface.** A logging GLD forwards every call to the software renderer and logs it, with its arguments, first in test programs and then in WindowServer. Compare with the installed ATI GLD. Result: a header with the GLD calls and structures WindowServer actually uses.
2. **Hybrid GLD.**
   - Still forward all drawing to the software renderer, but report an accelerated renderer with its video memory: at least 16 MB at record offset `+0x30` (`docs/gpu-tiger-gl-stack.md`).
   - The driver sets `IOGLBundleName` at runtime.
   - Find out whether WindowServer turns Quartz Extreme on and which GL operations its compositor uses.

   Everything stays correct, because the software renderer still draws.
3. **Take over the compositor's operations, one at a time:** rectangle textures and their upload (client storage), textured quads with blending, scissor, clears, then presenting the frame (resolve into the scanout buffer, flip at VBlank). In a check mode the GLD draws each frame both ways and logs differences.
4. **App GL windows:** complete the IOAccelSurface client so WindowServer can composite them.
5. **Pass:**
   - System Profiler reports Quartz Extreme.
   - Moving, overlapping and resizing windows works, as do the Dock effects and Exposé.
   - Frame times are measured.
   - No differences in check mode.

## Phase 5: OpenGL for applications (mostly R0)

1. Fixed-function pipeline as generated shaders (transform, lighting, texture generation, texture combine, fog, alpha test), with a shader cache.
2. Vertex data: immediate mode, vertex arrays, VBOs, index buffers, format and byte-order conversion.
3. Textures: formats and tiling, mipmaps, cube maps, DXT compression (native on Xenos), rectangle textures, render-to-texture, framebuffer objects.
4. Raster state: depth, stencil, blending, polygon offset, lines and points, occlusion queries, `glReadPixels`, `glCopyTexImage`.
5. Programmable shaders: ARB vertex and fragment programs translated to Xenos shaders. GLSL depends on what GLEngine passes to the GLD (found out in 4.1).
6. Everything unsupported goes to the software renderer for that draw call, so applications keep working.
7. Conformance: GL demos, Apple's OpenGL samples and a test subset that builds on Tiger, compared image by image with the software renderer. Then several contexts, shared objects, multi-threaded apps with six CPUs, and Core Image and Core Video.

## Phase 6: integration (R4, once or twice; milestone 8)

1. `XenosGPU` replaces XenonCoreGraphics in the mkext. Install the GLD and plug-ins in `/System/Library/Extensions`. Put all properties into the repository plists (no more pack-time plist changes). Remove the old test selectors.
2. Desktop smoke test with acceleration on:
   - windows: move, overlap, resize;
   - 720p and 1080p switching;
   - six CPUs;
   - a two-hour uptime test with GL load;
   - Ethernet and audio regression checks.
3. Merge into `main` after a passing test and the owner's OK.

## Risks

- The GLD interface is undocumented; phase 4.1 may take longer than planned. The 2D plug-in from phase 3 gives a faster desktop either way.
- WindowServer may require something Xenos does not offer in the way Tiger expects. Phase 0.1 should find this before the larger work starts.
- The EDRAM size makes full-screen depth buffers need tiling. That costs time in phase 2.4, but the WindowServer compositor needs no depth.
- Every crash needs the owner (power cycle, then XeLL from Aurora). Phases 1 and 3 need the most care: bad command buffers are caught by the address check and the hang recovery.
