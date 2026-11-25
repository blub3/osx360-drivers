# Xbox physical memory and a GPU heap (step 0.3)

Measured on 2026-11-25 with `XenosGPUProbe` (hand-loaded, `/Library/OSX360Dev`) on profile `main6`, Mac OS X 10.4.11.

## Physical memory map

| Range | Size | Used by | Source |
| --- | --- | --- | --- |
| `0x00000000`–`0x01FFFFFF` | 32 MiB | Tiger, DRAM bank 0 (kernel data up to `0x00B1B000`) | boot arguments |
| `0x02000000`–`0x047FFFFF` | 40 MiB | not available; owner unknown | device tree `/memory` |
| `0x04800000`–`0x04FFFFFF` | 8 MiB | available, unused by Tiger | device tree |
| `0x05000000`–`0x055FFFFF` | 6 MiB | not available; owner unknown | device tree |
| `0x05600000`–`0x07DFFFFF` | 40 MiB | available, unused by Tiger | device tree |
| `0x07E00000`–`0x080FFFFF` | 3 MiB | OpenBIOS (parking loop at `0x07E00060`) and its hash table at `0x08000000` | `ofmem.c` |
| **`0x08100000`–`0x0FFFFFFF`** | **127 MiB** | **available, unused by Tiger** | device tree |
| `0x10000000`–`0x1EFFFFFF` | 240 MiB | Tiger, DRAM bank 1 | boot arguments |
| `0x1F000000`–`0x1FFFFFFF` | 16 MiB | framebuffers (Tiger's linear one at `0x1F000000`, tiled scanout 8 MiB above it) | XenonFramebufferTest log |

- **Device tree `/memory`:** `reg` is `0x00000000`, 512 MiB. `available` has three ranges: `0x04800000` (8 MiB), `0x05600000` (40 MiB) and `0x08100000` (367 MiB), the last running into Tiger's bank 1.
- **Why the gap:** OpenBIOS (`arch/ppc/xbox360/macosx/macosx.c`) gives Tiger only the two banks above. Its comments are "XNU must not use the top 16MB" and, for the 32 MiB bank, "This reduces freezes, but they still may occur".
- **Tiger's memory:** `hw.memsize` is 272 MiB, with 37–53 MiB free after login (`vm_stat`).

## Contiguous allocations at runtime

`IOBufferMemoryDescriptor` with `kIOMemoryPhysicallyContiguous`, 9–17 minutes after boot, five loads:

| Size | Result |
| --- | --- |
| 16 MiB | Pass every time: one physical run at `0x1D0F2000`–`0x1E1E8000`, 32–37 ms |
| 32 MiB | Fails ("no memory") every time |
| 64 MiB | Fails ("no memory") every time |
| 128 MiB | Fails ("no memory") every time |

**A hand-loaded GPU driver cannot get a large heap from Tiger.**

## The unused range `0x08100000`–`0x0FFFFFFF`

- **Before the test:** all 2,032 samples (one word per 64 KiB) were non-zero, and 1,682 were `0xFFFFFFFF`. That looks like untouched RAM, not a live structure.
- **Pattern test:**
  - The probe mapped the range uncached through `IODeviceMemory`.
  - It wrote a pattern to every 256th byte: 520,192 words in 88 ms, 0 mismatches on read-back.
  - At unload, 2.5 minutes later, still 0 mismatches. That window included a WindowServer restart and a 10 MiB Ethernet round trip.
- **Retention:** a second load 30 s after the first unload found all 520,192 words intact. The pattern stays in RAM after unload, and each probe load reports how much of it is intact, so retention over hours of the same boot is measured by loading the probe again.
- **Afterwards:** no panic; desktop, Ethernet and audio fine.

**Conclusion:** this range is a candidate GPU heap of 127 MiB that needs no firmware change and no allocation from Tiger. What is still open:

- **Retention over a long uptime and under GPU load:** reload the probe after hours of uptime.
- **The unexplained OpenBIOS comment about freezes:** it concerns Tiger's own banks, and the gap may be part of the workaround.
- **Who owns the two "not available" ranges below it.**
- **Formal reservation:** a later firmware change (step 0.5) should mark the heap as reserved in the device tree, so nothing else ever claims it.

## Rules learned

- **Hand-loaded kexts link against the running kernel**, and `kextload -n -t` does not show every missing symbol: it accepted the probe, but the real load failed on `___udivdi3`. **Do no 64-bit division in kernel code** (the kernel exports no `__udivdi3`). Divide in 32 bits.
- **Kexts in `/Library/OSX360Dev`** are never loaded at boot. They load with `sudo kextload <path>` and unload with `sudo kextunload -b <bundle id>`, with the desktop up or with the GUI down (`tools/host/xbox-gui.sh stop`). All of this worked here.
