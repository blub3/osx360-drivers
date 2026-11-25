# Panic path and GPU interrupt (step 0.4)

Read from the Tiger 10.4.11 kernel (`/mach_kernel`, xnu-792.24.17, MD5 `def99c6ef0231ea3c2a627dd26201371`, disassembled on the G5) and the XenonPlatform source, 2026-11-25. No hardware change.

## What a panic does today

`panic()` → `Debugger()` (`osfmk/ppc/model_dep.c`), in this order:

1. `print_backtrace`; the panic text and backtrace are in `debug_buf`.
2. `packAsc` packs the text in place (7-bit ASCII packing). The length is limited to `0x7F8` (2,040) bytes.
3. **`PESavePanicInfo(debug_buf, length)`** calls the platform expert's virtual `savePanicInfo(UInt8 *buffer, IOByteCount length)` (vtable offset `0x5A4`).
   - XenonPE derives from `IODTPlatformExpert` and does not override it.
   - `IODTPlatformExpert::savePanicInfo` asks `IODTNVRAM::savePanicInfo`, which returns 0 without an NVRAM panic image (none on this console). It then falls back to `IOPlatformExpert::savePanicInfo`, which also returns 0.
   - **The panic text is lost.**
4. **If `savePanicInfo` returned a non-zero length and `panicDebugging` is 0:**
   - the other CPUs get `SIGPdebug` and stop;
   - then `draw_panic_dialog` draws the grey panic dialog into the console (linear) framebuffer;
   - then `PEHaltRestart(kPEHangCPU)` follows.

   That ends in `IOPlatformExpert::haltRestart`, which spins in place (`b .`) for `kPEHangCPU`: no reboot. On the way, `IODTPlatformExpert::haltRestart` first calls the NVRAM entry's sync.
5. **If the length is 0:** no CPU is stopped and no halt is called; `draw_panic_dialog` still runs.

No boot-args are needed for any of this, and no kernel patch.

## Plan: keep the panic text on screen

A reboot always lands in the Aurora dashboard, and RAM probably does not survive it. The text therefore has to be readable on the hung console: a photo by the owner.

- **The hook:** `XenonPE::savePanicInfo(buffer, length)` override (XenonPlatform, part of the `gpudev` boot in step 0.5). It must not take locks, allocate or call IOLog.
  - Unpack the 7-bit text (the reverse of `packAsc`).
  - Draw it with a built-in 8 × 8 font, scaled ×2 (120 columns × 67 lines at 1080p), directly into **both** framebuffers:
    - the linear one at `0x1F000000`;
    - the tiled scanout buffer 8 MiB above it, using the tiling formula from `XenonPE::refreshFramebuffer`.

    At panic time no timer refreshes the scanout any more, so writing only the linear buffer may never show.
  - First wait (bounded) until the GPU ring is idle, so a queued resolve cannot overwrite the text.
  - Also copy the packed text, with a magic word and its length, to the first page of the unused RAM at `0x08100000` (`docs/gpu-memory-map.md`). Reading it back after the next boot tests whether RAM survives a restart through Aurora.
  - Return the length, so that the kernel stops the other CPUs (important with SMP) and hangs.
- **Font:** a public-domain 8 × 8 font (for example `font8x8_basic`), not a GPL console font, because the repository is BSD-licensed.
- **Test:** a deliberate `panic()` from a hand-loaded test kext. It needs the owner for the power cycle and XeLL afterwards, so it is done once, when the owner is present.
- **Later option:** send the text over Ethernet from the hook. That needs a polled, lock-free transmit path in XenonEthernet.

## The GPU interrupt

- **The vector:** Xenos raises CPU vector **`0x58`** (`kXenonVectorXenos` in `XenonPlatform/src/Interrupts/XenonICRegs.hpp`; libxenon `PRIO_GRAPHICS`). It does not come through the PCI bridge: `sXenonCPUInterruptMap` has no bridge IRQ for it, so no driver can register for it today.
- **Today on thread 0:** `XenonInterruptController` **panics** on purpose if thread 0 takes `0x58` (`panic("Got Xenons here!")`). Secondary threads drop it as a stray vector.
- **It is live:** during SMP stage 1a, a secondary thread with interrupt priority 0 received `0x58` (`docs/platform-log.md`). The GPU raises it, and it is apparently delivered to a thread other than 0, since thread 0 has never panicked. Which register sets the target thread is not known yet.
- **Interrupt sources on the GPU side** (register indices from `adreno_common.xml.h` and `a2xx.xml.h`, byte offset = index × 4):
  - `CP_INT_CNTL`/`CP_INT_STATUS`/`CP_INT_ACK` (`0x1F2`–`0x1F4`): ring done (`RB_INT`), software interrupt (`SW_INT`, for fences), indirect-buffer done, and error interrupts for an opcode error, an indirect-buffer error, a protected-mode error and reserved bits. The error interrupts are a built-in fault detector for the hang recovery in step 1.5.
  - `RBBM_INT_CNTL`/`RBBM_INT_STATUS`/`RBBM_INT_ACK` (`0x3B4`–`0x3B6`): display update, GUI idle, read error.
  - `MASTER_INT_SIGNAL` (`0x3B7`): which block raised the interrupt.
  - `MH_INTERRUPT_MASK` (`0xA42`): memory hub errors, including an MMU page fault bit. Whether Xenos has a usable GPU MMU is open; it would simplify mapping user memory.
  - The VBlank interrupt of the display controller is not in these headers. XenosController uses no interrupts today, only timers.

## Plan: route the GPU interrupt

Part of the `gpudev` boot (step 0.5), because XenonPlatform is in the mkext:

1. Replace the panic for `0x58` in `XenonInterruptController` with a normal dispatch to a vector registered by the Xenos device. Publish it as an interrupt of the Xenos PCI nub, the way the bridge interrupts are published.
2. Keep it disabled until a driver enables it, and keep the stray-vector counter for secondary threads.
3. **Step 1.2** (hand-loaded `XenosGPU`, no reboot) then:
   - finds the target-thread setting with the debug client;
   - enables `CP_INT_CNTL` `SW_INT`/`RB_INT` and the error interrupts;
   - checks with the interrupt counters that `0x58` arrives on thread 0 and is acknowledged (`CP_INT_ACK`, then the interrupt controller's EOI).

**Until the platform change is booted, no hand-loaded driver may enable a GPU interrupt source:** if vector `0x58` ever reaches thread 0, the system panics.
