# SMP design for Xenon on Mac OS X 10.4.11

Goal: run XNU on more than one of Xenon's six hardware threads. Stage 1 uses one thread per core (3 CPUs). Stage 2 uses all six threads. The feature stays off by default and is enabled through the XenonPE personality, because this firmware cannot pass boot-args.

All facts below were measured on the Xbox 360 Slim with Tiger 10.4.11 or read from its `/mach_kernel` (MD5 `def99c6ef0231ea3c2a627dd26201371`). Hardware results are in `docs/platform-log.md`.

## Measured starting point

| Fact | Value | Source |
| --- | --- | --- |
| Where secondary threads wait | Physical `0x07E00060`, in OpenBIOS's `b .` loop | XeLL enters threads 1–5 at image offset `0x60`; OpenBIOS is linked at `0x07E00000` |
| Reachable from a running Tiger | Yes; all five counted in a loop and parked again | `XenonSMPProbe` release mode |
| Free space for release code | `0x07E00064`–`0x07E000FF`; the old `0xC00` vector area is also unused | Probe read |
| Thread state while parked | `MSR` `0x9000000002003000`: 64-bit, hypervisor, real mode, `ME`, `FP`, `VEC` | `XenonSMPProbe` SPR mode |
| `HID0`, `HID1`, `HID4`, `HID6`, `LPCR`, `HRMOR` | Identical on all six threads, `HRMOR` = 0 | SPR mode |
| Time base | Synchronized on all six threads; no offset beyond read latency (1–12 µs) | SPR mode |
| Kernel `MSR` on thread 0 | `0x1000000000009030`: hypervisor, 32-bit, `EE`, `ME`, `IR`, `DR` | SPR mode |

XeLL already configures the secondary threads like thread 0, so the libxenon per-thread setup (`HID6`, SLB, LPCR, `HRMOR`) is not needed before entering XNU. TLB invalidation is still prudent before translation is turned on.

## How XNU starts a CPU (from the Tiger kernel)

1. `cpu_start` (`0x93454`) writes the reset handler at physical `0xF0`: type 1 (start), `call_paddr` = `__start_cpu` (`0x92160`), argument = the new CPU's `per_proc` address. Then it calls `PE_cpu_start`, which calls `XenonCPU::startCPU(start_paddr = 0x100, arg_paddr)`, and waits for the new CPU to report in.
2. The reset vector at `0x100` saves `r13` and `r11`, finds type 1 at `0xF0`, clears it, loads `call_paddr` and the argument, and branches there with `r3` = argument.
3. `__start_cpu` loads the `per_proc` address, **writes the time base** from `per_proc + 0x98/0x9C` (`mttbl`/`mttbu` at `0x9218C`), and joins the common start path at `0x921E4`.
4. Later `cpu_sync_timebase` (`0x93C78`) asks the boot CPU for its time base through an IPI and **writes the time base again** (`0x93CE0`).

## Design

### 1. Release routine (written by XenonPlatform at `0x07E00064`)

Each parked thread runs a small loop: read its own mailbox word at `0x07E000C0 + PIR * 8`. While the word is zero, branch back to `0x60`. When the word is set, invalidate its TLB with `tlbiel`, then branch to physical `0x100`, XNU's reset vector, in its current real-mode hypervisor state. `startCPU` writes the mailbox for the target PIR only, so other threads stay parked.

The loop replaces the `b .` at `0x60` once, at XenonPE start, and only after checking the exact words found by the probe. Threads that are never started keep spinning in the same place as today.

### 2. No time base writes

Both XNU writes would set a shared, already-synchronized clock back by the startup latency. Measured synchronization makes them unnecessary.

- `startCPU` points `ResetHandler.call_paddr` (`0xF4`) at a trampoline in the OpenBIOS area. The trampoline runs `__start_cpu`'s first ten instructions without the three `mttb` instructions, then branches to `0x921E4`. XenonPlatform resolves `__start_cpu` by exact name and checks its 16 instruction words before using it. Any difference disables SMP.
- `cpu_sync_timebase` is routed to a no-op with the existing `routeFunction`, after checking its prologue words.

### 3. Changes in XenonPlatform

- `XenonCPU::start`: count CPUs from the enabled thread set, not `1`. Set `_isBootCPU` from the `state` property instead of always `true`, which is the current bug. Register each CPU with `ml_processor_register` (`start_paddr` = `0x100`).
- `XenonCPU::startCPU`: install the trampoline address and write the thread's mailbox.
- `XenonCPU::initCPU(false)`: enable this CPU's interrupt controller block and IPIs.
- `XenonCPU::signalCPU`: write the target thread's bit to the IPI generation register (`kXenonICRegIPIGeneration`).
- `XenonInterruptController`: program each thread's block at `kXenonICBase + PIR * 0x1000`. Today only block 0 is used, and device interrupts stay routed to CPU 0 in stage 1.
- CPU nubs: OpenBIOS creates one `/cpus` node. Stage 1 creates the extra CPU nubs in XenonPE to avoid a firmware change. Moving them into OpenBIOS on `claude/smp` remains possible later.
- Import rule: resolve any new kernel function at runtime with `resolveKernelSymbolExact`. Link-time imports must stay identical to the booting build.

### 4. Patcher fixes required before a second CPU runs user code

- `wrapPmapEnter` patches after the mapping is entered, so another CPU could run unpatched `dcbz` in that window. Patch first (on the physical page), then enter the mapping.
- `flushPatchCount` and the Emulate64 counter rely on disabling interrupts, which is not enough with two CPUs. Use an `IOSimpleLock` for the counters. The Emulate64 counter becomes approximate, which the log must state.
- The pending commpage-hook install must happen once, under a lock.

### 5. GPU code

The owner gets a list of GPU code that assumes one CPU (for example `refreshFramebuffer` disabling interrupts only locally) before any SMP build runs WindowServer. Claude does not change GPU code.

## Stages and pass criteria

| Stage | Threads | Pass criteria |
| --- | --- | --- |
| 1a | PIR 0 + PIR 2 (core 1) | `hw.ncpu` = 2, both CPUs take timer interrupts, a parallel build finishes, 1 hour uptime, no regressions |
| 1b | PIR 0, 2, 4 | `hw.ncpu` = 3, same checks, multi-hour uptime |
| 2 | All six | `hw.ncpu` = 6, same checks |

Every stage keeps the default boot unchanged. A gated profile on the boot USB has its own rollback, as with the patcher work.
