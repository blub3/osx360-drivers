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

## First hardware test: profile `audirq` (2026-11-25)

`smp3a` (three CPUs) with XenonAudio `ce363791e581ae0b81259206442ace42` and `XenonAudioInterrupt`. Profile `audirq`: `AUDIRQ.ELF` (MD5 `3bd89f2ba26d15c457ec3a1670a1238b`) + `XAIQ.MKE` (MD5 `cd103198239fabd37244035d54c7eaa2`).

**Result: fail.** No storm, hang or panic, but **the owner heard crackles, dropouts and gaps.**

| Check | Result | Criterion |
| --- | --- | --- |
| Boot log | Interrupt mode enabled, channel 1 control `1D08001C` → `1C000003` | Pass |
| CPU 0 external interrupts, playing minus idle | +121 per second | 50–250: pass |
| Decrementer ticks, all CPUs, playing | 1,652/s against 819/s idle | At most twice idle (1,638): fail by 14/s |
| Five `say` runs ("Audio test after the restart.", 1.81 s of audio) | 3.29–3.30 s each (3.99 s with the timer build) | 3.0–5.0 s: pass |
| After playback | 90 external interrupts per second on CPU 0 | No storm: pass |
| Listening (owner) | Crackles, dropouts and gaps | **Fail** |

Compared with the timer build on the same system, playback added 833 decrementer ticks per second instead of about 5,750.

Suspects: the `0x1C` acknowledge written every descriptor (only bit 3 is known to be the descriptor status), and the position fix, which lets Tiger's engine erase behind the real position. Next: profile `audknb` with test switches (`XenonAudioTestKnobs`, `tools/audio/xenon-audio-set`) to compare acknowledge `0x08`/`0x1C`, position fix on/off and timer/interrupt mode in one boot.

## Listening rounds with `audknb` (2026-11-25)

Profile `audknb`: XenonAudio `e6f57823f78f913716225317f073f392` with `XenonAudioInterrupt` and `XenonAudioTestKnobs`, `AUDKNB.ELF` (MD5 `b5302bec0a8837924d82900b2e174827`) + `XAKB.MKE` (MD5 `c2f786cb855a914f4f5db5f4e673a5b0`). Sentence: "The quick brown fox jumps over the lazy dog, while the Xbox plays this sentence for the listening test.", three times.

| Round | Mode | Result |
| --- | --- | --- |
| A | Timer, original position code (the old driver's behaviour) | Clean, but the owner heard "fox jumps over the lazy dog, while the Xbox plays this sentence for the listening test test test": **the start is missing and the end repeats** |
| B | Interrupt, acknowledge `0x08`, original position code | **Kernel panic** (grey screen) as soon as playback started; the Xbox went off the network |

**Round A explains the glitches.** Tiger's IOAudioEngine assumes playback starts at frame 0, but XenonAudio never restarts the DMA channel: it continues at whatever descriptor it stopped on, so the start of a sound lands behind the play head. After the end, the original code reports a wrong position (control instead of state register), so the engine never erases the played part and the 341 ms buffer keeps looping ("test test test"). With the position fix, the engine erases behind the real position, which is not where it expects it, which fits the dropouts and gaps of the first interrupt test.

**Round B, second panic.** Acknowledging only bit 3 probably left another status (starved) set, so the level line stayed active and CPU 0 took the same interrupt continuously. The storm guard (`disableInterrupt` from the filter) did not stop it. No panic text: the Xbox was off the network and has no NVRAM. After a power cycle, the running driver was switched to timer mode and the stick's default set back to `smp3a` (`kboot.conf.before-revert-smp3a.bak` keeps the previous file).

Interrupt mode stays off until both panics are understood and the storm guard masks the line in the bridge itself.

## Next: restart the channel at every engine start (`audrst`, built, not booted)

XenonAudio `fbb13929625d5840146a16fee695f41b`: with `XenonAudioResetOnStart`, `startAudio` re-runs `setupSampleDescriptors` (clear the buffer, rewrite the descriptors, restart at descriptor 0) and resets the wrap detection. Profile `audrst`: timer mode only (no `XenonAudioInterrupt`), with the switches; `AUDRST.ELF` (MD5 `47d9255901ee5b14e2a08c0e3607a259`) + `XARS.MKE` (MD5 `7b1a05d5fde12b8450b8ab9975d83764`). Listening plan: reset on with the original position code, then reset on with the position fix; both should play the whole sentence once, without the repeated end.
