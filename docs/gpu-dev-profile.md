# Development boot profile `gpudev` (step 0.5)

Built and staged 2026-11-25 as the stick's default; **booted and tested the same evening** (results at the end). Booting it needs the owner, because every restart lands in the Aurora dashboard.

## What it is

`gpudev` is the tested `main6` set with only these changes:

| Kext | Change |
| --- | --- |
| XenonCoreGraphics | **Left out.** The hand-loaded `XenosGPU` (phase 1) takes the GPU instead. Until then XenonPlatform's CPU refresh draws the desktop (every 500 ms), so the desktop is slower than on `main6`. |
| XenonFramebufferTest | Same binary. Plist as tested, without `IOGLBundleName` and `IOCFPlugInTypes`, because the GLD and the WindowServer plug-in need XenonCoreGraphics. |
| XenonPlatform | New build from this branch, same kernel imports as the tested build. Personality as tested (six CPUs registered, `XenonSMPDeferStart`, `XenonSMPIdleLow`, `XenonPmapFastScan`) plus `XenonPanicScreen`, `XenonGPUInterrupt` and `XenonGPUHeap`. |
| XenonPCI, XenonAudio, XenonEthernet, XenonSATA, XenonSMC, IOGraphicsFamily 1.4.2 | Byte-identical to `XMN6.MKE` |

| File | MD5 | Made from |
| --- | --- | --- |
| `GPUDEV.ELF` | `31bcbd83797b49e75f49dbed1427ef2d` | `WINOVR.ELF`, mkext name `XWOV.MKE` → `XGDV.MKE` (2 bytes differ) |
| `XGDV.MKE` | `cdddf867bda5b42c39323edec16f4363` (331,360 bytes) | `XMN6.MKE` with `tools/host/mkext_edit.py` |
| XenonPlatform binary | `9b71124e6f00dde001ca378363d16a85` | — |
| XenonPlatform plist | `e5dbfadb621922c03769f9038bcefca7` | — |
| XenonFramebufferTest plist | `ae5d263443b5145f141de6cea82e12ae` | — |

The files are kept on the G5 in `~/osx360-build/gpudev/`; build output is not committed. Tiger's offline link check (`kextload -n -t -z`) accepted the new XenonPlatform on the Xbox.

## The three new XenonPlatform features (`XenonPE_Panic.cpp`)

Each is off unless its personality key is true, so `main6` and every other profile behave as before.

### `XenonPanicScreen`

- **What it does:** overrides `savePanicInfo`, unpacks Tiger's packed panic text, and draws it with the kernel's console font (`_iso_font`, looked up at runtime) into the linear and the tiled scanout framebuffers.
- **Panic record:** a copy goes to a record at `0x08100000` (magic `XPNC`).
- **Return value:** it returns the length, so the kernel stops the other CPUs and hangs without rebooting (`docs/gpu-panic-and-interrupts.md`).
- **At boot:** XenonPE logs whether a record from before the boot is still in RAM.
- **Test without a panic:** the platform functions `XenonPanicScreenTest` and `XenonPanicScreenTestEnd`.

### `XenonGPUInterrupt`

- **What it does:** XenonInterruptController no longer panics when thread 0 takes Xenos vector `0x58`.
  - If a driver registered the vector (OpenBIOS already gives `xenos@2` `interrupts` = `0x58`), it is dispatched to that driver.
  - If not, the GPU's CP and RBBM interrupt sources are masked and acknowledged at the GPU, and a counter is increased.
- **What it does not change:** which thread the GPU targets (today a secondary thread, PIR 2).

### `XenonGPUHeap`

- **What it does:** after checking that `0x08100000`–`0x0FFFFFFF` overlaps none of Tiger's DRAM banks, XenonPE publishes `XenosGPUHeapBase` = `0x08200000` and `XenosGPUHeapSize` = 126 MiB on itself, for `XenosGPU`.

## Staging

- Copy `GPUDEV.ELF` and `XGDV.MKE` to the stick and add `gpudev="uda0:/GPUDEV.ELF"` to `kboot.conf` (13 entries).
- Back it up as `kboot.conf.before-gpudev.bak`, and keep the checksum snapshots before and after, as for `main6`.
- `kboot.conf` uses `timeout=0`, so the default boots at once. To boot `gpudev`, the default has to be `gpudev`.
- **If it does not come up:** put the stick in a PC and set `default=main6` again, or copy `kboot.conf.before-gpudev.bak` back.

**Staged 2026-11-25:**

- **Before staging:** the stick was unchanged since `main6`.
- **Copied:** `GPUDEV.ELF` and `XGDV.MKE`, both with matching checksums.
- **`kboot.conf`:** saved as `kboot.conf.before-gpudev.bak` (MD5 `a41e29c7d49247c7b8d26ca5b10ede11`, `default=main6`). The new `kboot.conf` (MD5 `96bcae00c60045605711670df3d57ed7`) adds `gpudev` as the first entry and the default; 13 entries.
- **Check:** the snapshots before and after (`session-baseline/2026-11-23/osx360boot.{before,after}-gpudev.md5`) differ only in these four files.

## Boot test (with the owner)

Pass criteria, fixed before the boot:

1. **Boot:** desktop with autologin and SSH. `kextstat` lists seven Xenon kexts (no XenonCoreGraphics). `hw.ncpu` 6, `hw.availcpu` 1. No panic.
2. **`system.log` from XenonPE:**
   - "GPU heap 0x8200000-0xFFFFFFF (126 MiB) published";
   - "Panic screen armed" with a non-zero font address and "record mapped";
   - "No panic record from before this boot";
   - "GPU vector 0x58 is dispatched to its driver on thread 0".
3. **Regression:** 10 MiB Ethernet round trip with matching checksums, `say` heard by the owner, and the owner can use the desktop (slow redraw is expected).
4. **Panic screen test (no reboot):** load `XenosGPUProbe` with `XenosGPUProbePanicScreenTest` set in its plist. The owner sees the test text, readable and the right way round, for 8 s, and then the desktop again.
5. **Optional, only with the owner's OK:** load the probe with `XenosGPUProbePanic`.
   - The owner photographs the panic screen, power-cycles the Xbox and starts XeLL.
   - At the next boot, XenonPE's log line says whether the panic record survived.

## Boot test results (2026-11-25, 22:31 boot)

| Check | Result |
| --- | --- |
| 1. Boot | Pass. Desktop with autologin; the owner confirmed it is very slow, as expected. SSH works. Seven Xenon kexts, no XenonCoreGraphics. `hw.ncpu` 6, `hw.availcpu` 1. No panic. |
| 2. XenonPE log | Pass. "GPU heap 0x8200000-0xFFFFFFF (126 MiB) published", "Panic screen armed: font 0x36e51c, record mapped at 0x8100000" (the kernel's `_iso_font` address), "No panic record from before this boot", "GPU vector 0x58 is dispatched to its driver on thread 0". |
| 3. Regression | Ethernet: pass. 10 MiB with matching checksums both ways, 7.0 s up and 3.5 s down over SSH, slower than `main6` because of the CPU refresh. `say` returned 0; the owner's confirmation of the audio is pending. Idle CPU load is about 24% system time, from the CPU framebuffer refresh. |
| 4. Panic screen test | Pass. `XenonPanicScreenTest` from 22:35:14 to 22:35:23; the owner read the text, readable and the right way round, so the font's bit order is correct. Then the desktop came back and the probe unloaded. `kextload` returns before the probe's 8 s wait ends, so the probe can only be unloaded afterwards. |
| 5. Deliberate panic | Not done (optional, needs the owner's OK and a power cycle). |

**RAM after a shutdown:** the probe's heap pattern from before the shutdown was gone (0 of 516,096 words intact). A powered-off Xbox keeps nothing in RAM. A panic record could at most survive a restart, which is untested.
