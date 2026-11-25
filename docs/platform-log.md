# OSX360 platform log

Non-GPU platform work on Mac OS X 10.4.11 for the Xbox 360 Slim. GPU results stay in the GPU tracker.

## Next step

Task 1 (pmap dcbz patcher) is merged into `main` (`ae01a6d`). Open follow-ups: a faster path for the CoreGraphics fill routines, and a Finder launch measurement.

Task 2: SMP, branches `claude/smp` in both repos (OpenBIOS from `codex/usb2-ssd-boot`). Design: `docs/smp-design.md`.

**Stage 1a works, including start during boot** (`smp1c`, commit `c6155da`): `XenonSMPThread2` starts PIR 2 as CPU 1. `XenonSMPDeferStart` instead registers it stopped, to be started later with `sudo ./xenon-cpu-start 1` (`tools/smp`, installed in the Xbox home folder).

1. Test `smppark` (built, not staged): stop CPU 1 and start it again, three rounds (both CPUs available again, CPU 1 takes no decrementer ticks while stopped, Ethernet checksums). Then measure Ethernet with CPU 1 running, stopped and running again in the same boot, unencrypted with `nc` and over SSH.
2. Fix the SMC lock hold (audit finding 2) in a separate build.
3. The display error the owner saw at two-CPU shutdown is still unexplained. Tiger's shutdown does not stop CPUs (`halt_all_cpus` calls `PEHaltRestart` directly), so it is not the empty `quiesceCPU`.
4. Then stage 1b (PIR 0, 2, 4). Its idle loop must not take issue slots from the other thread on the same core.

Gate everything behind a personality flag, because the firmware cannot pass boot-args. Keep the import rule below.

Rules learned: never import a kernel symbol in XenonPlatform that `pmapfast` does not already import. Tiger's offline link check accepts such builds, but they fail at boot. Resolve extra kernel functions at runtime with `resolveKernelSymbolExact`.

The Xbox is on `smp1c`; its rollback selects `smpdef3`, whose rollback selects `pmapdef`. SMP driver audit: `docs/smp-driver-audit.md`. The owner decides when to restore `gpuwinoverlap`.

## Build notes

- The G5 has no `git`. Sync source with `rsync` to `~/osx360-build/osx360-drivers`.
- The Makefile expects Darling. On the G5, override its tools and use the local wrapper `~/osx360-build/bin/g5-cc`, which links with `ld_classic`:

```sh
make -C XenonPlatform OSX_VERSION=tiger DARLING_SHELL= AS=/usr/bin/as \
  CC=$HOME/osx360-build/bin/g5-cc \
  CXX=/usr/bin/powerpc-apple-darwin9-g++-4.2.1 LD=/usr/bin/ld_classic
```

- Firmware passes no boot-args and the console has no working NVRAM. Test builds enable a feature with a personality property in the test mkext, for example `XenonPmapStats`.

## 2026-11-25: `smppark` build, CPU 1 can be stopped again (not booted)

Built on the G5, link-time imports identical to `pmapfast`. XenonPlatform MD5 `6069f8e71250249bcb2c4bc7b0f1e9a7`. Profile `smppark`: `SMPPARK.ELF` (MD5 `96dee5bd8edf9745bdfc61a6a9fff320`) + `XSPK.MKE` (MD5 `18ee08df08b09ea32f05cc2aa780e0b6`), with `XenonSMPThread2` and `XenonPmapFastScan`. All other kexts in the mkext are byte-identical to `XWOV.MKE`.

- Interrupt barrier (audit finding 1): `sync` before `interruptActive = 0`.
- Stopping a secondary CPU (audit finding 3). Tiger's `IOCPU::setProperty` (`0x2E299C`) accepts only lower-case `IOCPUState` values: `"running"` calls `processor_start` on a stopped CPU, `"stopped"` calls `haltCPU` on a running one. `haltCPU` calls `processor_exit` (resolved at runtime). Tiger then runs `processor_offline` → `cpu_sleep`, which tail-calls `quiesceCPU` on the CPU going offline and panics with "zombie processor" if it returns. `quiesceCPU` now blocks the thread's interrupt priority (`0x7C`), sets `SleepState` in its `per_proc` (which `cpu_exit_wait` waits for) and returns to the mailbox loop at `0x07E00064` in real mode with the MSR XeLL left there. A later start goes through the same release path as the first.
- `tools/smp/xenon-cpu-start <n> stop` (MD5 `d085ca0b62ac4f27b9f42983871f23d2`).

Finding while reading Tiger: shutdown and restart do not stop CPUs. `host_reboot` → `halt_all_cpus` → `PEHaltRestart`. The empty `quiesceCPU` therefore did not cause the display error at shutdown.

## 2026-11-25: SMP two-hour uptime test (`smp1c`)

Two CPUs from boot, display off, no user activity. A script on the Linux host checked the Xbox every minute and ran a load and Ethernet check every 10 minutes.

**Result: pass.** 2 h 1 min (06:18–08:19, uptime 2 h 7 min at the end).

| Check | Result |
| --- | --- |
| Heartbeats | 113, none missed |
| `hw.availcpu`, WindowServer, Finder, Dock | 2, running at every heartbeat |
| 10 MiB Ethernet up and down, 13 checks | Checksums match every time |
| CPU 1 external interrupts | 0–6 per second (IPIs only) |
| CPU 1 decrementer ticks | 177–499 per second |
| One busy process | 4.58–4.72 s |
| Two busy processes in parallel | 4.63–4.66 s for the first two checks, then 8.79–9.20 s |
| Panic log | None |

The parallel result changed at about 06:30, when the screensaver started: `ScreenSaverEngine` used about 83% of one CPU (44 minutes of CPU time by 07:22), and the load average stayed near 1.0. With that load on one CPU, two loops sharing the other take twice as long. Total CPU use was about 48%, so both CPUs were scheduled. The screensaver load made this a mixed-load test rather than an idle one.

## 2026-11-24: Ethernet slowdown investigation on two CPUs (`smp1c`)

Read-only measurements on the running two-CPU system, around 50 MiB transfers each way over SSH (5.13 MiB/s up, 5.52 down):

| Measure | Result |
| --- | --- |
| TCP data retransmitted, retransmit timeouts, bad checksums | 0 |
| Completely duplicate packets received | +34 over about 100 MiB |
| External interrupts, CPU 0 (all Ethernet) | +187,533 in about 19 s, about 9,900/s |
| External interrupts, CPU 1 (IPIs) | +1,824, about 96/s |
| `sshd` CPU during an SSH download | 93–99% of one CPU |

**Ruled out:** packet loss, driver errors and an IPI storm. **SSH throughput is limited by `sshd`'s encryption on one thread**, so it falls when that thread runs slower or waits longer. With two CPUs, `sshd` can run on CPU 1 while all Ethernet interrupts and the network stack run on CPU 0. Cross-core data movement or lock handoff in Tiger's network stack would fit that, but it is not measured.

An unencrypted HTTP comparison failed: the Python 2.3 HTTP server did not stay up when started over SSH, so no HTTP number was taken.

## 2026-11-24: SMP stage 1a starts the second CPU during boot

Profile `smp1c`: `SMP1C.ELF` (MD5 `5fdd5ed6513788c6f18ea50db78f9b16`) + `XS1C.MKE` (MD5 `f37da1fb64f499654397eb73c4b4de63`). Same XenonPlatform binary as `smpdef3` (`343a539d3665d6af20677955410a66a1`), with `XenonSMPThread2` and `XenonPmapFastScan` and without `XenonSMPDeferStart`.

**Result: pass.** This is the boot that hung as `smp1b` before the interrupt storm fix.

| Check | Result |
| --- | --- |
| Boot log | `SMP enabled for thread mask 0x5`, `released PIR 2` |
| `hw.ncpu` / `hw.availcpu` | 2 / 2 after boot |
| One busy process | 4.58 s |
| Two busy processes in parallel | 4.65 s and 4.66 s |
| CPU 1 over 3 s | 2 external interrupts, 1,244 decrementer ticks |
| WindowServer, Finder, Dock | Running, no panic log |
| Ethernet 50 MiB | Checksums match, 5.46 MiB/s up, 5.39 MiB/s down (still slow, see open items) |
| Tiger GPU files | Unchanged |

Audio, mouse and keyboard were confirmed by the owner on `smpdef3` with the same binary, not re-checked on this boot.

Open: while shutting down the previous two-CPU session (`smpdef3`), the owner saw an error mentioning a buffer on the desktop. `system.log` has no entry for it. `XenonCPU::quiesceCPU` and `haltCPU` are still empty, so shutdown does not stop CPU 1 cleanly. That is the first suspect.

## 2026-11-24: SMP stage 1a works, two CPUs

Branch `claude/smp`, commit `c6155da`. Profile `smpdef3`: `SMPDEF3.ELF` (MD5 `74abf14e8f848fa66930fd891016b7f4`) + `XSD3.MKE` (MD5 `f42bc9b0c4784482555df2fdb2d3a7a6`). XenonPlatform MD5 `343a539d3665d6af20677955410a66a1`, with `XenonSMPThread2`, `XenonSMPDeferStart` and `XenonPmapFastScan` set and link-time imports identical to `pmapfast`. Tiger's link check passed.

Pass criteria for stage 1a: `hw.ncpu` = 2; both CPUs run work; a stress test finishes; SSH, WindowServer, Finder, Dock, Ethernet, audio, mouse and keyboard keep working. The multi-hour uptime test is still open.

**Result: pass, except the uptime test and an Ethernet slowdown.** The owner confirmed audio, mouse and keyboard.

| Check | Result |
| --- | --- |
| Boot | Single CPU, `hw.ncpu` 2, `hw.availcpu` 1 |
| `sudo ./xenon-cpu-start 1` | `released PIR 2`, `hw.availcpu` 2 |
| One busy process | 4.59 s |
| Two busy processes in parallel | 4.76 s and 4.74 s |
| CPU 1 over 3 s | 779 DSI, 2 external interrupts (IPIs), 1,312 decrementer ticks, 1,431 system calls |
| 4 busy processes + 200 MiB of disk writes | All finished (about 122 s each), file MD5 correct, no panic |
| WindowServer, Finder, Dock | Running throughout |
| Ethernet, 3 runs | Checksums match; 5.52–5.65 MiB/s up, 5.80–5.91 MiB/s down (single CPU: 7.3–7.8 / 8.9–9.3) |

**Open: Ethernet is about 25–35% slower with two CPUs.** The cause is not measured yet.

## 2026-11-24: deferred SMP start, CPU 1 runs but storms on external interrupts

Profiles `smpdefer` (`5d20c9f`) and `smpdef2` (`c645908`): `XenonSMPThread2` + `XenonSMPDeferStart`. Both booted single-CPU with `hw.ncpu` 2 and `hw.availcpu` 1, and PIR 2 registered stopped. Tiger's `IOCPU::setProperties` only handles `IOCPUState` = `Stopped`, so `smpdef2` adds a `XenonCPU::setProperties` override.

On `smpdef2`, `sudo ./xenon-cpu-start 1` over SSH logged `Starting deferred CPU PIR 2`, `released PIR 2` and `processor_start returned … state 3`, and `hw.availcpu` became **2**. The system stayed up. But CPU 1 did no work: one busy process took 4.74 s, and two in parallel took 9.52 s and 9.60 s. The load average stayed at 1.00.

Per-CPU data via `/dev/kmem` (`PerProcTable` at `0x365000`; CPU 1's data at virtual `0x01161000`), diffed over 3 s. Counters from `hwCtr` at offset `0x800`:

| Counter | CPU 0 | CPU 1 |
| --- | --- | --- |
| DSI (`0x80C`) | +1,594 | 0 |
| External interrupts (`0x814`) | +40 | **+883,362** |
| Decrementer (`0x824`) | +394 | 0 |
| System calls (`0x830`) | +2,721 | 0 |

**CPU 1 is stuck in an external interrupt storm.** Likely cause: the secondary handler ended with EOI plus auto-update, which reopened PIR 2's block, and the Xenos vector held toward PIR 2 fired again at once. Fix: secondaries send a plain EOI and restore priority `0x74` themselves.

## 2026-11-24: SMP stage 1a retry hangs, IC priority probe

Profile `smp1b` (commit `37ebfeb`, secondaries at priority `0x74`): **fail.** The owner saw the boot hang before the desktop, and the Xbox did not answer ping or SSH. That boot left no `system.log` entries at all, so it stopped before the logger started. `pmapdef` was restored on this computer and booted normally.

IC priority probe (`XenonSMPProbe` IC mode, real-mode 64-bit access, on the running `pmapdef` system). CPU 0 sent IPIs to the parked PIR 3 and read PIR 3's acknowledge register:

| Priority | Vector `0x78` | Vector `0x70` |
| --- | --- | --- |
| `0x00`, `0x40` | delivered | delivered |
| `0x70` | delivered | latched only |
| `0x74`, `0x77` | delivered | latched only |
| `0x78`, `0x7C` | latched only | latched only |

**A vector is delivered only when it is above the block priority.** So `0x74` passes IPIs, and the `smp1b` hang is not an IPI filter problem. XeLL had already left PIR 3's block at ID 8, priority `0x74`.

Interpretation: in `smp1a`, PIR 2 died in its first interrupt. In `smp1b` it survived startup, and something in early boot then stalled with two CPUs. Next: start the secondary CPU after boot (`XenonSMPDeferStart`) and watch `system.log` live over SSH, so a failure leaves CPU 0 and its logs reachable.

## 2026-11-24: SMP stage 1a, second CPU starts, then panics on a GPU vector

Branch `claude/smp`, commit `70e2a45`. Profile `smp1a`: `SMP1A.ELF` (MD5 `c70dbdc9815a84606360970ea0551072`) + `XS1A.MKE` (MD5 `0b703b4ebd57dcf694927013158b4cfa`). XenonPlatform MD5 `b3164d6416bf4aa22c075bc49ea9e57c`, with `XenonSMPThread2` and `XenonPmapFastScan` set and link-time imports identical to `pmapfast`.

**Result: partial.** The release routine, the trampoline and XNU's start path all worked. The log shows `SMP enabled for thread mask 0x5` and `released PIR 2`, and `sysctl hw.ncpu` and `hw.availcpu` report 2. Tiger reached the desktop with SSH, WindowServer, Finder and Dock running. Right after the release, XNU logged `No debugger configured - dumping debug information`, and the owner saw the panic screen.

Diagnosis without a panic log (no NVRAM): the system kept running on CPU 0, so the panic returned. `/dev/kmem` showed `panicstr` cleared and `panic_caller` = `0x180214C8`. XenonPlatform loads at `0x1801F000` with its text one page in, so the caller is text offset `0x14C8`: the return from `panic("Got Xenons here!")` in `XenonInterruptController::handleInterrupt`. **PIR 2 received the Xenos vector `0x58`** because its interrupt block was opened with priority 0.

Fix: secondary threads use priority `0x74`, which admits only the IPI vector `0x78` (0 accepts all, `0x7C` blocks all, as in libxenon). Any other vector on a secondary is acknowledged and dropped instead of panicking.

## 2026-11-24: SMP thread state and time base probe

`XenonSMPProbe` SPR mode, loaded by hand on the running `pmapdef` system. Each parked thread stored its registers at `0x07E00C00 + PIR * 0x40`, and the area was restored afterwards. The system stayed up.

| Register | PIR 0 (Tiger) | PIR 1–5 (parked) |
| --- | --- | --- |
| `MSR` | `1000000000009030` | `9000000002003000` (64-bit, hypervisor, real mode) |
| `HID0` | `0` | `0` |
| `HID1` | `9c30104000000000` | same |
| `HID4` | `00003f0000000000` | same |
| `HID6` | `0001803800000000` | same |
| `LPCR` | `2` | `2` |
| `HRMOR` | `0` | `0` |

Time base: the five values, read back to back before PIR 0's own read, trailed it by 0x246, 0x1F7, 0x192, 0x93 and 0x33 ticks (pass 1: 0x1A3, 0x142, 0xE5, 0x9B, 0x32). The difference shrinks with read order, so it is read latency, not offset. **All six time bases are synchronized.** XNU's `__start_cpu` and `cpu_sync_timebase` both write the time base, so the design avoids both writes.

## 2026-11-24: SMP probe of the parked secondary threads

Branch `claude/smp`. `XenonSMPProbe` is a diagnostic kext, not part of the package. It was loaded by hand with `kextload` on the running `pmapdef` system and unloaded afterwards. Its results go to `system.log`, because `ioreg` stops at the IODTNVRAM entry before reaching it.

Background: XeLL enters a loaded image with thread 0 at offset `0x0` and the other threads at offset `0x60` (libxenon `startup_from_xell.S`). OpenBIOS is linked at `0x07E00000`, outside XNU's memory, and parks them in `b .` at `0x07E00060`. `/dev/mem` returns zeros for physical memory on this system, so userland cannot read it.

Read-only result: `0x07E00000` holds `480017f4 60000000 5f617267 00000001` (branch to OpenBIOS entry, XeLL argument marker). `0x07E00060` holds `48000000` (`b .`), and `0x64-0xFF` is empty.

Release probe: the probe wrote a counting loop at `0x64-0x80`, pointed `0x60` at it for 200 ms, then restored `b .`. Each thread counted at `0xC0 + PIR * 4`.

| PIR | After 100 ms | After 200 ms | Parked (+10 ms and +110 ms) |
| --- | --- | --- | --- |
| 0 (Tiger) | 0 | 0 | 0 |
| 1 | 0x071141 | 0x0C8178 | 0x0C8548, unchanged |
| 2 | 0x1907BE | 0x34EE1E | 0x34FF88, unchanged |
| 3 | 0x18FD50 | 0x34C25A | 0x34D3AC, unchanged |
| 4 | 0x1A443B | 0x313F4F | 0x316073, unchanged |
| 5 | 0x1A553C | 0x31865E | 0x31A792, unchanged |

**Result: all five secondary threads are alive at physical `0x07E00060` in real mode and can be redirected and parked again.** PIR 1 runs slower because it shares core 0 with Tiger. The system stayed up, with SSH, WindowServer, Finder and Dock running.

## 2026-11-24: default build test before merge

Branch `claude/pmap-patcher`, commit `3867c58`. Profile `pmapdef`: `PMAPDEF.ELF` (MD5 `e3d46967bd32f0f76c14acffe10e8c62`) + `XPDF.MKE` (MD5 `bf1817b9b00034e59634d297c238bb3b`, 437,664 bytes). XenonPlatform MD5 `3ac25cc11f8b0f7d25d01ca62136652e`, built from the repository `Info.plist` with only `XenonPmapFastScan` set. Tiger's link check passed.

Pass criteria, set before the run: Tiger boots; only the fast scan is configured; launches stay at fast-scan times; a 50 MiB Ethernet transfer keeps its checksum; audio is audible; Tiger GPU files unchanged.

**Result: pass.** The owner heard the audio and approved the merge.

| Check | Result |
| --- | --- |
| Boot, SSH, WindowServer, Finder, Dock | Pass, no panic or new crash reports |
| XenonPE settings | `XenonPmapFastScan = Yes` only |
| Warm launch medians | Terminal 1.09 s, TextEdit 1.33 s, Safari 1.71 s, Calculator 1.64 s |
| Ethernet up / down | 7.77 / 9.32 MiB/s, checksums match |
| Tiger GPU files | Unchanged |

Without statistics, scan counters are not published. The launch times match the fast-scan run, against 1.36–2.15 s without it.

## 2026-11-24: app launch times with and without the fast scan

Same XenonPlatform binary (`3ac25cc11f8b0f7d25d01ca62136652e`) in both runs, with the commpage hook and statistics on. Only `XenonPmapFastScan` differs. `pmapslow`: `PMAPSLOW.ELF` (MD5 `eb92668c947ebe2184d1a8ce59142876`) + `XPSL.MKE` (MD5 `3877cb5e870b7cf4b7476009c307d560`), fast scan off. `pmaprt2`: fast scan on.

Method: fresh cold boot for each run, no user activity, 20 s settle. Per app, one cold launch (first after boot) and five warm launches, timed from `tell application to activate` until the app answers Apple events. Scan time is the patcher's own counter during that interval.

Pass criterion from the brief: measurably faster app launch.

**Result: pass.** Warm launches are 10–18% faster, cold launches 8–17%.

| App | Cold off | Cold on | Warm median off | Warm median on | Warm saving | Scan per warm launch off / on |
| --- | --- | --- | --- | --- | --- | --- |
| Terminal | 1.84 s | 1.58 s | 1.36 s | 1.11 s | 0.25 s | 658 / 15.0 ms |
| TextEdit | 1.50 s | 1.38 s | 1.54 s | 1.36 s | 0.18 s | 494 / 12.4 ms |
| Safari | 2.28 s | 1.90 s | 2.15 s | 1.85 s | 0.31 s | 655 / 13.8 ms |
| Calculator | 2.32 s | 2.04 s | 1.82 s | 1.64 s | 0.19 s | 513 / 12.6 ms |

About 1 minute after power-on, scan time was 9.21 s without the fast scan and 0.28 s with it. The launch saving is smaller than the scan saving because some scanning happens after the app first answers. Each configuration was measured in one boot. Finder was not measured.

## 2026-11-24: executable mapping survey

Read-only survey on the running `pmaprt2` system with Terminal, TextEdit and Safari open: `vmmap` of all 47 processes, 1,461 executable regions.

| Executable regions | Result |
| --- | --- |
| File-backed images | 212, all Mach-O (thin or fat), 0 other files |
| Anonymous (`VM_ALLOCATE`) | Present in 2 processes |

**Conclusion: the data-rewrite case has no exposure in normal use.** It needs a non-Mach-O file mapped executable, and none was found. Anonymous executable pages are zero-filled when first mapped, so the map-time scan finds no data in them.

A fix would need the kernel to track which mappings come from Mach-O images, with invalidation when mappings change. A stale entry would leave real dcbz unpatched, which clears 128 bytes instead of 32. That risk outweighs the measured exposure, so this stays a documented limit.

## 2026-11-24: commpage hook stage two passes

Profile `pmaprt2`: `PMAPRT2.ELF` (MD5 `1dcd23a14b0e67eb9656a3ae11868aa0`) + `XPR2.MKE` (MD5 `a1b83b3f61d5dcbdc4d82200afc9065d`, 437,664 bytes). Same XenonPlatform binary as `pmaprt1` (`3ac25cc11f8b0f7d25d01ca62136652e`), with the flush entry branch enabled. Tiger's link check passed.

Pass criteria, set before the run: Tiger boots; status 1; generated `dcbz` through the official flush clears 32 bytes; a clean range stays unchanged; no hook failures; apps launch; a 50 MiB Ethernet transfer keeps its checksum; audio is audible.

**Result: pass.** The owner heard the audio.

| Check | Result |
| --- | --- |
| Boot, SSH, WindowServer, Finder, Dock | Pass, no panic or new crash reports |
| Hook status | 1 |
| Generated `dcbz`, official `sys_icache_invalidate` | 32 bytes (128 before the hook) |
| Generated `dcbzl` | 128 bytes |
| Clean 4 KB range, unaligned and zero-length flushes | 0 changed |
| Terminal, TextEdit and Safari launch and quit | Pass |
| Hook calls from boot and these launches | 0, apart from the test's 2 words, with 0 failures |
| Ethernet up / down | 7.56 / 9.09 MiB/s, checksums match |
| Tiger GPU files | Unchanged |

No Tiger program in this sample generated `dcbz` code at runtime. Code flushed directly with `icbi` is still not covered.

## 2026-11-24: commpage hook stage one passes

Branch `claude/pmap-patcher`, commit `32db185`. Profile `pmaprt1`: `PMAPRT1.ELF` (MD5 `6bcc03c76e3643b68f3bff3265f01c0f`) + `XPR1.MKE` (MD5 `a7344a62a77e2bf2280c03b5908b57a6`, 437,696 bytes). XenonPlatform MD5 `3ac25cc11f8b0f7d25d01ca62136652e`, with link-time imports identical to `pmapfast`. Stage one: stub and machine call installed, flush entry unchanged.

Pass criteria, set before the run: Tiger boots; status 5; flush entry unchanged; the stub called directly makes generated `dcbz` clear 32 bytes and `dcbzl` 128; a clean range stays unchanged.

**Result: pass.** SSH, WindowServer, Finder and Dock run, with 0 fast-scan mismatches and unchanged Tiger GPU files. **The boot failures came from the link-time imports.**

| Check | G5 | Xbox |
| --- | --- | --- |
| Status | n/a | 5 |
| Flush entry `0xFFFF8520` | n/a | `7c842379`, unchanged |
| Generated `dcbz`, stub called directly | 32 bytes | 32 bytes, word patched |
| Generated `dcbzl`, stub called directly | 128 bytes | 128 bytes, word patched |
| Clean 4 KB range, unaligned and zero-length flushes | 0 changed | 0 changed |
| Hook counters | n/a | 2 calls, 2 words, 0 failures |
| Generated `dcbz`, official flush (not hooked yet) | 32 bytes | 128 bytes |

## 2026-11-24: BSD dependency test fails

Profile `pmapbsd`: the same binary as `pmapsym` (MD5 `4ece2e83391510dcfe674b2c3781c8a0`) with `com.apple.kpi.bsd` 8.0.0 added to `OSBundleLibraries`. Not committed. `XPBS.MKE` 435,264 bytes (MD5 `768ca72538c5657f4044030322c95172`), `PMAPBSD.ELF` MD5 `352a20bc7c3d8119151db578103b65de`.

**Result: fail.** The owner reported that the cold boot failed. Declaring the BSD KPI does not make these imports safe for XenonPlatform. Instead of narrowing which symbol is at fault, the hook now resolves `copyin` and `copyout` at runtime by exact name and no longer uses `OSIncrementAtomic`. Its link-time imports must match the booting `pmapfast` build exactly.

## 2026-11-24: symbol test fails

Profile `pmapsym`: the `pmapfast` code plus unused references to `copyin`, `copyout` and `OSIncrementAtomic`. Not committed. XenonPlatform MD5 `4ece2e83391510dcfe674b2c3781c8a0`, `XPSY.MKE` 435,232 bytes (MD5 `d6f35b3564b1e4cbb2babbf764d5730e`), `PMAPSYM.ELF` MD5 `077b2ca881f50c38e55dde1777e65e08`. Tiger's `kextload -n -t -z` accepted it.

**Result: fail.** The owner reported that the cold boot failed. **Importing these symbols is enough to break the boot, even though the code never runs.**

Likely cause: XenonPlatform declares only `com.apple.kernel.iokit`, `libkern` and `mach` 1.1. The GPU kexts that import `copyin` also declare `com.apple.kpi.bsd`. The boot-time linker probably cannot resolve `copyin` and `copyout` for XenonPlatform, so the platform expert does not load. Tiger's offline link check does not catch this. It may also explain earlier unexplained boot failures after a kext gained new imports. Next test: the same build with a `com.apple.kpi.bsd` dependency.

## 2026-11-24: size test boots

Profile `pmappad`: the `pmapfast` code (`68a141c`) plus 8 KB of unused, incompressible data. Not committed, built in `~/osx360-build/pad` on the G5. XenonPlatform 111,472 bytes (MD5 `65582f4198fde1209fd4ed4627043668`). `XPPD.MKE` 444,352 bytes (MD5 `c8efd4bc1def9b620566027562b6427b`), `PMAPPAD.ELF` MD5 `1d43fa5ae63d2006b05a51e7db7ba075`. Tiger's link check was skipped because the Xbox was down. The G5 unpacked the archive, and all other kexts are byte-identical.

**Result: pass.** SSH, WindowServer, Finder and Dock run, with 0 fast-scan mismatches and unchanged Tiger GPU files. **Binary or archive size alone does not cause the failure.**

Compared with this working code, the failing binary imports `_copyin`, `_copyout` and `_OSIncrementAtomic`, and it has a larger `__text` section (0x7F28 against 0x756C). Next test: the `pmapfast` code with unused references to those three symbols.

## 2026-11-24: hook-off boot failure

Profile `pmapnof`: `PMAPNOF.ELF` (MD5 `4e33079bf18b61d0f5470edac541db93`) + `XPNF.MKE` (MD5 `9313aa4a8ca85acf1c5324c48ac9f1ba`, 437,248 bytes). Same XenonPlatform binary as `pmapchk` (MD5 `4d8bef5784d88d0a094295bdb21214b1`), with `XenonPmapFlushPatch` absent, so no hook code runs.

**Result: fail.** The owner reported that the cold boot failed. The crash does not depend on running the hook. It follows the build itself.

XenonPlatform binary sizes: 97,656 (`pmapstat`), 100,724 (`pmaptrap`) and 103,240 bytes (`pmapfast`) boot; 107,612, 108,000, 108,360 and 108,692 bytes fail. Next test: the `pmapfast` code (`68a141c`) with 8 KB of unused padding. If it fails, size is the trigger.

## 2026-11-24: commpage hook check-only boot failure

Branch `claude/pmap-patcher`, commit `874326e`. Profile `pmapchk`: `PMAPCHK.ELF` (MD5 `6b8223bd751c170645c2df9c5cc4ab46`) + `XPCK.MKE` (MD5 `1b1f102d063105b488003f96e5cc5635`, 437,280 bytes). XenonPlatform binary MD5 `4d8bef5784d88d0a094295bdb21214b1`. This build runs the install checks and writes nothing.

**Result: fail.** The owner reported that the cold boot failed. The installer's writes are therefore not required for the failure.

Archive sizes so far: `XPMF.MKE` 435,136 bytes boots; `XPMC.MKE` 436,960, `XPS1.MKE` 437,152 and `XPCK.MKE` 437,280 fail. All occupy 107 pages. The remaining suspects are the read-only install checks and the larger XenonPlatform binary.

## 2026-11-23: commpage hook stage one boot failure

Branch `claude/pmap-patcher`, commit `995ab7a`. Profile `pmapst1`: `PMAPST1.ELF` (MD5 `1a35a0003055066862e8d5889364a60f`) + `XPS1.MKE` (MD5 `a90cae07669798812bb7d482d7b957c5`). XenonPlatform binary MD5 `91010d34fc4f96b639c87b039d7cb2f7`. This build installs the stub and machine call and leaves the commpage flush routine unchanged.

**Result: fail.** The owner reported that the cold boot crashed. Tiger's logs have no entry for it, and there is no `panic.log` or new crash report, so it died before the logger started.

Recovery on 2026-11-24: `kboot.conf` was restored from `kboot.conf.before-pmapst1.bak` on this computer, then `pmapfast` cold booted with SSH, WindowServer, Finder and Dock running, 0 fast-scan mismatches and unchanged Tiger GPU files.

The owner sees a brief memory-like pattern just before the desktop appears. It coincides with WindowServer opening the framebuffer. The owner has seen it before these patcher builds, so it is not caused by them. Before the boot, all original `OSX360BOOT` files except the intentionally changed `kboot.conf` matched the session baseline, and the Tiger GPU files were unchanged.

## 2026-11-23: commpage flush hook boot failure

Branch `claude/pmap-patcher`, commit `b1848dc`. Profile `pmapflush`: `PMAPFLSH.ELF` (MD5 `c53fa6433bca45008c5c6f7d191f710e`) + `XPMC.MKE` (MD5 `8a556ce5f21bc77b7f59287a6be999ee`). XenonPlatform binary MD5 `4892fb515ac703006724ee5c7a794328`.

**Result: fail.** Two cold boots ended with block corruption on the display and no ping or SSH. Tiger's `system.log`, `wtmp` and crash logs have no entry for either boot, and there is no `panic.log`. The hook installs before the logger starts, so the missing logs do not clear it.

Recovery: `kboot.conf` was restored from `kboot.conf.before-pmapflush.bak` on this computer, then `pmapfast` cold booted with SSH, WindowServer, Finder and Dock running and 0 fast-scan mismatches.

Checks on the running `pmapfast` system: `commPagePtr32` is `0x1A2DA000`, the expected flush words are present there, the stub area is zero, and machine-call slots 14 and 15 are empty. The archive uses 107 pages, the same as the working `XPMF.MKE`. The cause is not identified.

The block pattern may be a general sign of an early hang, because `refreshFramebuffer` converts the console image into the tiled scanout. This is an inference and has not been tested.

## 2026-11-23: text-only patching survey

Read-only survey of `/System`, `/usr`, `/bin`, `/sbin`, `/Applications` and `/Library` on the Xbox: 4,454 PowerPC Mach-O slices. Home folders were not scanned.

| Location of words matching the patch mask | Words | Meaning |
| --- | --- | --- |
| `__TEXT,__text` | 680 | Code section |
| `__TEXT,__textcoal_nt` (libGLImage) | 24 | Code section, flagged as instructions |
| `__TEXT,__const` (GLEngine) | 39 | Inside named code routines such as `gleBlockClear` |
| `rw-` segments and `__LINKEDIT` | 55 | Not mapped executable in normal use |

**Conclusion: a `__TEXT,__text`-only patcher would be unsafe.** It would miss the 63 real or code-like matches in the other two sections, and each missed `dcbz` would clear 128 bytes instead of 32.

Exposure test on the running `pmapfast` profile, with the G5 as reference:

| Case | G5 | Xbox |
| --- | --- | --- |
| Word in `rw-` `__DATA` | Unchanged | Unchanged |
| Word in `rw-` anonymous memory | Unchanged | Unchanged |
| Data word in a file mapped `r-x` | Unchanged | **Rewritten** |
| Same word through `read()` while cached | Unchanged | **Rewritten** |
| `dcbz` generated in an already-mapped `rwx` page | Clears 32 bytes | **Clears 128 bytes** |

Both Xbox failures also apply to the original patcher logic. Whether a rewritten file-backed page can reach the disk was not tested.

No-execute probe: private pages mapped `rw-`, `r--`, `r-x` and `rwx` all executed on both the Xbox and the G5. The patcher only scans mappings entered with execute permission, so code run from a `rw-` or `r--` page is not patched either.

Hardware controls, from the Cell Broadband Engine Programming Handbook v1.11 (sections 6.1.6.4 and the alignment interrupt section). This documents the related Cell PPE, not Xenon itself:

- `dcbz` always operates on the 128-byte cache line. No 32-byte mode is documented, and `HID5` does not appear.
- `dcbz` raises an alignment interrupt only on caching-inhibited storage or with the L1 data cache disabled (`HID4[en_dcway] = 0000`). Neither is practical system-wide.
- `HID4[force_ai]` covers misaligned load/store cases and does not list `dcbz`.

## 2026-11-23: dcbz trap cost

User-space benchmark on the running `pmapfast` profile, 1,000,000 iterations, best of 3 runs. The G5 is the behaviour reference.

Pass criteria, set before the run: `dcbz` clears exactly 32 bytes and `dcbzl` exactly 128, matching the G5; each emulated instruction causes one trap.

**Result: pass.** Both machines cleared 32 and 128 bytes with 0 wrong bytes. Each 3-run set took exactly 3,000,000 traps, and the plain-store runs took 0.

| Operation | G5 | Xbox |
| --- | --- | --- |
| `dcbz` (32 bytes) | 6.0 ns | 1,365.9 ns, emulated |
| `dcbzl` (128 bytes) | 10.0 ns | 1,405.5 ns, emulated |
| 8 word stores (32 bytes) | 12.8 ns | 34.2 ns |
| 32 word stores (128 bytes) | 55.3 ns | 95.1 ns |

On the Xbox, an emulated `dcbz` costs about 40x the equivalent stores, and `dcbzl` about 15x. At about 1.4 µs per trap, the 1,291,720 traps in the first 2 minutes cost about 1.8 s. This is an estimate from a tight loop and an upper bound on the trap count. The benchmark added 2 patched words to the running system's counter.

## 2026-11-23: fast dcbz page pre-scan

Branch `claude/pmap-patcher`, commit `68a141c`. Profile `pmapfast`: `PMAPFAST.ELF` (MD5 `013d06693230d7d62c91d245c62e1987`) + `XPMF.MKE` (MD5 `ade8d960f1474cc310620d2c9d9957e9`). XenonPlatform binary MD5 `459481437fec38789a366cdce9b52184`. All other kexts are byte-identical to `XWOV.MKE`.

Pass criteria, set before the run: Tiger boots; SSH, WindowServer, Finder and Dock run; 0 mismatches on sampled clean pages; the same 41 words are patched; clean-page scan cost is under 80 µs; a 50 MiB Ethernet transfer keeps its checksum; audio is audible.

**Result: pass.**

| Measure | `pmaptrap` | `pmapfast` |
| --- | --- | --- |
| Pages scanned, about 2 minutes after power-on | 29,853 | 29,926 |
| Scan time | 12.11 s | 0.30 s |
| Average per page | 405 µs | 10 µs |
| Terminal launch scan time | 0.38 s | 0.010 s |
| TextEdit launch scan time | 0.30 s | 0.008 s |
| Words patched | 41 | 41, same addresses and values |
| Sampled clean pages rechecked | n/a | 1,871, 0 mismatches |
| Ethernet up / down | 7.27 / 8.92 MiB/s | 7.84 / 9.17 MiB/s |

The fast-scan time excludes the sampled rechecks, which exist only in statistics builds. Real app launch time was not measured.

## 2026-11-23: dcbz trap counter and word classification

Branch `claude/pmap-patcher`, commit `187302c`. Profile `pmaptrap`: `PMAPTRAP.ELF` + `XPMT.MKE` (MD5 `173591843f35f806152e4f352d93f748`). XenonPlatform binary MD5 `f98b5bc99754af99a13a9a74fc8c4a40`. All other kexts are byte-identical to `XWOV.MKE`.

Pass criteria, set before the run: Tiger boots; SSH, WindowServer, Finder and Dock run; the trap counter is present and app launches still work; the log holds one entry per patched word; a 50 MiB Ethernet transfer keeps its checksum; audio is audible.

**Result: pass.** Ethernet ran at 7.27 MiB/s up and 8.92 MiB/s down with matching checksums. The owner heard the audio.

| Measure | Value |
| --- | --- |
| Emulator traps about 2 minutes after power-on | 1,234,696 (upper bound on dcbz traps) |
| Terminal launch | up to 83,936 traps |
| TextEdit launch | up to 191,094 traps |
| Patched words | 41: 35 `dcbz`, 6 `dcbzl` |
| Location | All in CoreGraphics `__TEXT,__text` |
| Functions | `CGSFillDRAM64` 34, `vec128CGSFill8by1` 4, `vec128CGSFillDRAM64` 2, `CGSFillDRAM8by1` 1 |

No false positives occurred in this sample. Emulate64 clears 32 bytes for `dcbz` and 128 bytes for `dcbzl`, matching G5 behaviour.

File reads return the patched words while the page is cached, so a checksum of a patched library can differ from its on-disk bytes. The patcher logged unpatched originals after each cold boot, so these runs show no sign of patches reaching the disk.

## 2026-11-23: dcbz patcher counters

| Item | Value |
| --- | --- |
| Branch and commit | `claude/pmap-patcher`, `50964c4` |
| Profile | `pmapstat`: `PMAPSTAT.ELF` + `XPMS.MKE`, derived from `gpuwinoverlap` |
| Changed kext | XenonPlatform 0.0.5, binary MD5 `20299c5dcbc08060a54f34efaf7c4a83` |
| Other kexts | Byte-identical to `XWOV.MKE`, including XenonCoreGraphics 0.0.5d48 |
| `XPMS.MKE` MD5 | `26be692a767be2bd77c086b86fa4f2aa` |
| `PMAPSTAT.ELF` MD5 | `e4712d89231fbeed89725fc9daacea6a` |

Pass criteria, set before the run: Tiger boots; SSH, WindowServer, Finder and Dock run; all four counters appear and rise on app launch; a 50 MiB Ethernet transfer keeps its checksum; audio is audible.

**Result: pass.**

| Check | Result |
| --- | --- |
| Cold boot, SSH, WindowServer, Finder, Dock | Pass, no panic log |
| Counters | Pass, all four rise on app launch |
| Ethernet, 50 MiB each way | Checksums match, 7.18 MiB/s up, 8.96 MiB/s down |
| Audio | Pass, speech heard by the owner |

Patcher cost in the first 2 minutes after power-on:

| Measure | Value |
| --- | --- |
| Pages scanned | 36,885 |
| Pages changed | 4 |
| Words patched | 41 |
| Scan time | 14.75 s (737,686,672 ticks at 50 MHz), about 400 µs per page |
| Scan time at 46 s uptime | 9.47 s (22,877 pages) |
| Terminal launch | 2,157 pages, 0.83 s |
| TextEdit launch | 807 pages, 0.30 s |

Open points:

- The counters do not show whether the 41 patched words are real instructions or data.
- `ioreg -l` stops with "can't obtain properties" at the `options` (IODTNVRAM) entry. The original `gpuwinoverlap` profile does the same, so this test did not cause it.

Session end: `gpuwinoverlap` restored and cold booted. SSH, WindowServer, Finder and Dock run, and the counters are absent. All original `OSX360BOOT` files and Tiger GPU files match their session-start checksums. `XPMS.MKE`, `PMAPSTAT.ELF` and `kboot.conf.before-pmapstat.bak` remain on the USB as additions.
