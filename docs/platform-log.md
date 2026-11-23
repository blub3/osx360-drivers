# OSX360 platform log

Non-GPU platform work on Mac OS X 10.4.11 for the Xbox 360 Slim. GPU results stay in the GPU tracker.

## Next step

Task: pmap dcbz patcher, branch `claude/pmap-patcher`.

1. Patch only Mach-O text ranges, so data words that look like dcbz are never changed.
2. Measure real app launch times with and without the fast scan.
3. Evaluate a faster path for the CoreGraphics fill routines, which cause most dcbz traps.

The Xbox is on `pmapfast`. The owner decides when to restore `gpuwinoverlap`.

## Build notes

- The G5 has no `git`. Sync source with `rsync` to `~/osx360-build/osx360-drivers`.
- The Makefile expects Darling. On the G5, override its tools and use the local wrapper `~/osx360-build/bin/g5-cc`, which links with `ld_classic`:

```sh
make -C XenonPlatform OSX_VERSION=tiger DARLING_SHELL= AS=/usr/bin/as \
  CC=$HOME/osx360-build/bin/g5-cc \
  CXX=/usr/bin/powerpc-apple-darwin9-g++-4.2.1 LD=/usr/bin/ld_classic
```

- Firmware passes no boot-args and the console has no working NVRAM. Test builds enable a feature with a personality property in the test mkext, for example `XenonPmapStats`.

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
