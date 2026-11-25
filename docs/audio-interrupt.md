# Xenon audio interrupt

Goal (brief backlog): replace XenonAudio's 200 µs polling timer with interrupt-driven position updates. Nothing public documents how the audio block acknowledges its interrupt: libxenon polls, and the Free60 Linux kernel only routes the line (PCI bridge IRQ 12, CPU vector `0x44`). Everything below was measured on the Xbox 360 Slim with Tiger 10.4.11 and `XenonAudioProbe` (hand-loaded, not in the package) on 2026-11-25, running `smp3a` (three CPUs).

## Cost of the polling timer

| State | Decrementer ticks/s (CPU 0 / 1 / 2) | CPU idle |
| --- | --- | --- |
| Idle | 461 / 194 / 196 | 94% |
| `say` playing | 2,057–2,086 / 2,218–2,303 / 2,334–2,499 | 84–86% |

About 5,000 extra timer interrupts per second while the engine runs, spread over all CPUs.

## Registers (audio BAR0 at physical `0xEA001600`, length `0x40`)

Two DMA channels with the same layout, 16 bytes each. XenonAudio and libxenon use channel 0 only.

| Offset | Channel 0 | Channel 1 |
| --- | --- | --- |
| `+0x00` / `+0x10` | Descriptor list address | `0x01ED1100`, inside Tiger's RAM (left by firmware) |
| `+0x04` / `+0x14` | State: bits 0–4 read pointer, bits 8–12 write pointer ("last valid"), bit 15 status, bits 16–31 bytes left in the current descriptor | `0x00009F1F`: read = write pointer, nothing left |
| `+0x08` / `+0x18` | Control: bit 24 Run; bits 2–4 written as `0x1C` at init; low bits read back as status | `0x1D08001C`: Run set |
| `+0x0C` / `+0x1C` | Format, `0x1C` | `0x02008406` |
| `+0x20`–`+0x34` | Values changing about 375 times per second (two changing 24-bit values with counters), probably output monitors | |

Each descriptor covers `0x800` bytes (512 frames, 10.7 ms at 48 kHz). The bytes-left field counts down in steps of `0x40` every ~0.33 ms.

Bridge register for IRQ 12 (`0xEA000040`): `0x00400000` while masked. With the line routed, it reads `0x01C00891`; bit 24 is set while the line is active.

## The interrupt

Method: the probe routed IRQ 12 to parked thread PIR 3 (external interrupts off, never takes them) and read PIR 3's acknowledge register every ~116 µs.

1. **The line was active in every sample**, while playing and while idle (15,000 of 15,000 samples). This is the "storm" in XenonAudio's TODO.
2. **Channel 1 holds it.** Clearing channel 1's Run bit released the line (1 of 200 samples pending). Channel 1 had stalled at its end with Run set.
3. **Channel 0, while playing, holds it too** until acknowledged. Writing `0x00`, `0x04` or `0x10` into the low Control byte (Run kept) did not change that.
4. **Control bit 3 is the per-descriptor completion status, cleared by writing 1.** Writing `0x08` into the low byte each time the line was active gave one short assertion per descriptor: gaps of 87–96 samples (10.7 ms), each at the start of a new descriptor (bytes left `0x0800`, read pointer +1).
5. When the read pointer reached the write pointer (starved), the line stayed active and `0x08` did not clear it; writing `0x1C` did.

Side effect: restoring channel 1's original Control value (Run set) started channel 1 reading descriptors from Tiger's RAM. It stopped by itself; the probe then cleared its Run bit.

## Panic during the probe session

During the probe session CPU 1 (PIR 2) stopped: 0 decrementer ticks and 0 interrupts, while CPUs 0 and 2 kept running, so SSH still worked. The owner saw the grey "You need to restart your computer" screen. The audio timer ran on CPU 1, so audio stalled and `say` processes hung in the kernel. Tiger recorded nothing: `panicstr`, `panic_caller`, `debug_buf_ptr` were 0, `debugger_cpu` was −1, and CPU 1's machine-check and reset counters were 0. No NVRAM, so no `panic.log`. **Cause unknown.** Main suspect: the probe's real-mode interrupt controller access (data translation off), tested before only with one CPU, now possibly running on CPU 1. The probe must not run again until this is understood. After a cold boot, three CPUs and audio worked normally.

## Design for the driver

- Behind a personality flag, timer remains the default and the fallback.
- At start: clear channel 1's Run bit, so it no longer holds the line.
- Keep the write pointer one descriptor behind the read pointer, so channel 0 never starves.
- Interrupt: on vector `0x44`, write channel 0's Control low byte with `0x1C` (Run kept) to acknowledge, then do what the timer does (time stamp on wrap, write pointer).
- Storm guard: if the interrupt rate goes far above one per descriptor, disable it and fall back to the timer.
- Fix `getAudioPosition`, which reads Control instead of State.
