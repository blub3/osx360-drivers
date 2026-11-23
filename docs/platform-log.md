# OSX360 platform log

Non-GPU platform work on Mac OS X 10.4.11 for the Xbox 360 Slim. GPU results stay in the GPU tracker.

## Next step

Task: pmap dcbz patcher, branch `claude/pmap-patcher`.

1. Add a dcbz emulation trap counter. This needs a hook in the kernel's emulation path.
2. Classify the patched words as real code or data, so false positives can be measured.
3. Design the "already scanned" record, including invalidation when a physical page is freed or refilled.

## Build notes

- The G5 has no `git`. Sync source with `rsync` to `~/osx360-build/osx360-drivers`.
- The Makefile expects Darling. On the G5, override its tools and use the local wrapper `~/osx360-build/bin/g5-cc`, which links with `ld_classic`:

```sh
make -C XenonPlatform OSX_VERSION=tiger DARLING_SHELL= AS=/usr/bin/as \
  CC=$HOME/osx360-build/bin/g5-cc \
  CXX=/usr/bin/powerpc-apple-darwin9-g++-4.2.1 LD=/usr/bin/ld_classic
```

- Firmware passes no boot-args and the console has no working NVRAM. Test builds enable a feature with a personality property in the test mkext, for example `XenonPmapStats`.

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
- `ioreg -l` stops with "can't obtain properties" at the `options` (IODTNVRAM) entry. Not yet compared on the original profile.
