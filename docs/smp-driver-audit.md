# SMP driver audit (2026-11-25)

Read-only review of the drivers on `claude/smp` for code that assumes one CPU. Stage 1a runs PIR 0 and PIR 2. Device interrupts are routed to PIR 0 only, and PIR 2 takes only IPIs. Work-loop threads, user threads and IOKit calls can run on either CPU.

Covered: `XenonPlatform` (interrupt controller, CPU, SMP, patcher), `XenonEthernet`, `XenonSATA`, `XenonSMC`, `XenonAudio`, `XenonPCI`. GPU code was only read and is listed separately for the owner. The Tiger kernel functions quoted below were disassembled from `mach_kernel.tiger` on the G5.

## Findings

| # | Where | Issue | Effect | Risk | Proposed change |
| --- | --- | --- | --- | --- | --- |
| 1 | `XenonInterruptController::handleInterrupt`, end of the device path | No barrier before `vector->interruptActive = 0`. Tiger's `IOInterruptController::enableInterrupt` (`0x2E70FC`) clears `interruptDisabledSoft`, spins until `interruptActive` is 0, then reads `interruptDisabledHard` and calls `enableVector`. If CPU 1 sees `interruptActive = 0` before CPU 0's `interruptDisabledHard = 1` store, or before the bridge mask write, the source stays masked. | An Ethernet, SATA, USB or audio interrupt could stay masked for good: the device stalls. | Low probability, severe | `sync()` before `vector->interruptActive = 0`. One instruction, all CPUs. |
| 2 | `XenonSMC::writeMessage` | Spins for the SMC FIFO with `_lock` held and interrupts off, for up to `kXenonSMCTimeout` = 10,000,000 × 1 µs (10 s). `filterInterrupt` takes the same lock on CPU 0. With one CPU, interrupts were simply held off. With two, CPU 0 spins on the lock at interrupt level while CPU 1 waits. Tiger's simple locks also have a deadlock timeout (value not checked yet). | Only if the SMC stays busy: CPU 0 stalls, or the kernel may panic on the lock timeout. | Low probability, severe | Wait for FIFO ready without the lock (or with a short bound), take the lock only to write the message. |
| 3 | `XenonCPU::quiesceCPU`, `haltCPU` | Empty. `IOCPUState = "stopped"` reaches them for CPU 1. | `haltCPU` is called but does nothing; an empty `quiesceCPU` would end in Tiger's "zombie processor" panic. Shutdown and restart do not stop CPUs (`halt_all_cpus` calls `PEHaltRestart` directly), so this is not the cause of the display error at shutdown. | Known | Park the thread again: interrupt priority `0x7C`, `SleepState`, back to the OpenBIOS mailbox loop. Done in `smppark`. |
| 4 | Emulate64 trap counter (`lwz`/`addi`/`stw` at physical `0x5210`) | Not atomic. Both CPUs run Emulate64. | The counter loses counts. Diagnostics only. | Accepted | Already documented in `smp-design.md`. Say "approximate" wherever the value is reported. |
| 5 | `routeFunction` | Writes four instruction words. If the target function ran on another CPU during the write, that CPU could execute a half-written jump. | None today: all five routes (`Debugger`, `pmap_enter`, `grade_binary`, `grade_cpu_subtype`, `cpu_sync_timebase`) are installed in `XenonPE::start`, before any secondary starts. | Rule | Route functions only before secondaries start. |
| 6 | `XenonEthernet::sendTxPacket` | No `eieio`/`sync` between filling the descriptor and setting the owner bit, and none between the cacheable descriptor stores and the doorbell MMIO write. Not specific to SMP: one CPU writes all of it. | The controller could read a stale address or length. Not seen: 0 retransmits, every checksum matched. | Low, pre-existing | `eieio` before the owner store, `sync` before the doorbell. Owner's call; separate from SMP. |
| 7 | `XenonSMC::getRTC` | `_rtcSyncer` is not cleared after `wait()`, which releases the syncer. A late duplicate GetRTC reply would signal a freed object. | Crash on a duplicate reply only. | Low, pre-existing | Clear `_rtcSyncer` under `_rtcLock` after the wait. |

## Checked and safe with two CPUs

- **Ethernet, SATA, SMC secondary handlers, audio timer:** all run on their work loop. `IOGatedOutputQueue` makes `outputPacket` take the same gate as the interrupt action, so the TX ring is serialized.
- **Interrupt entry:** `interruptActive = 1; sync; isync` before reading `interruptDisabledSoft` pairs with Tiger's `soft = 0; sync; isync; spin on active`. `atInterruptLevel` is `ml_at_interrupt_context` (Tiger `0x2DFB48`), so `disableInterrupt` from the handler does not spin on itself.
- **PCI config access:** `IOSimpleLockLockDisableInterrupt` around every access.
- **SMC HDMI I2C and RTC:** `IOLock` plus `_lock`; flags set in the filter and read in the work loop are benign races (a second button press during handling can be merged).
- **Audio:** `takeTimeStamp` uses `mach_absolute_time`; the time base is synchronized on all threads.
- **Patcher:** pages are patched before `pmap_enter` and synced with `pmap_sync_page_data_phys`. Two CPUs patching the same page write the same words. Statistics and flush-hook counters use `IOSimpleLock`. The flush hook is installed under its lock before the first user code runs.
- **IPIs:** `signalCPU` writes the sender's own block. Two CPUs signalling at once use different registers.

## GPU code (for the owner, not changed)

- `XenonPE::refreshFramebuffer` (`XenonPE_GPU.cpp`) uses `ml_set_interrupts_enabled(false)` as its lock. It is called from the console path and from `wrapDebugger` on a panic. With two CPUs, two callers can copy at the same time, and the other CPU keeps drawing while a panic is shown. A simple lock would serialize callers; the panic path may need to skip the lock.
- `XenonCoreGraphics` has no locks or interrupt handlers today. Anything it adds later must not use "interrupts off" as a lock.

## Ethernet slowdown (25–35% with two CPUs)

Nothing in the driver explains it: the TX/RX paths are serialized and device interrupts stay on CPU 0. Likely causes outside the driver, to measure:

1. **Thread hand-offs across CPUs.** Each packet passes the interrupt (CPU 0), the Ethernet work loop, Tiger's `dlil_input_thread` and sshd. With one CPU these are direct switches. With two, they can land on different cores, and data moves between the cores' L1 caches. CPU 1 takes only about 96 IPIs per second, so the idle CPU probably polls instead of sleeping (`supports_nap = false`).
2. **Mutex spinning.** Tiger's `lck_mtx` spins while the owner runs on another CPU, which only happens with two CPUs.
3. **sshd is CPU-bound** (93–99% of one CPU), so any extra kernel cost per packet shows directly as lower throughput.

Measurement plan for one boot of `smpdef3` (CPU 1 starts later): unencrypted `nc` transfers in both directions, before and after starting CPU 1, recording interrupts and IPIs on both CPUs (`hwCtr` at `per_proc + 0x800`, CPU 0 at `0x366000`).

**Stage 2 warning:** if the idle loop polls, an idle PIR 1 would take issue slots from PIR 0 on the same core. Stages 1b and 2 need an idle loop that lowers the thread's priority (`or 1,1,1`) or stops the thread.

## Suggested order

1. Findings 1 and 3 in one build (`smppark`: interrupt barrier, stop and restart CPU 1). Test: boot, stop/start rounds, Ethernet with and without CPU 1.
2. Finding 2 (SMC lock).
3. The Ethernet measurement above, then stage 1b.
4. Findings 6 and 7 with the owner, as ordinary driver fixes.
