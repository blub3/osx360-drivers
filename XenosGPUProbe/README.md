# XenosGPUProbe

Diagnostic kext for step 0.3 of the GPU plan (`docs/gpu-driver-plan.md`). It is not part of the package and is never loaded at boot.

- Attaches to the Xenos PCI device next to XenosController and lists the other drivers on it.
- Logs the device tree `/memory` ranges and the DRAM banks OpenBIOS gave Tiger.
- Tries physically contiguous allocations of 16, 32, 64 and 128 MiB.
- Tests the GPU heap in RAM Tiger does not manage, `0x08200000`–`0x0FFFFFFF` (XenonPE keeps its panic record at `0x08100000`): reports how much of an earlier pattern is still intact, writes a pattern to every 256th byte and checks it again at unload. It writes nowhere else and touches no GPU register.

Two switches in the personality, both off: `XenosGPUProbePanicScreenTest` draws XenonPE's panic screen for 8 s without a panic (profile `gpudev`); `XenosGPUProbePanic` panics on purpose, which then needs the owner (power cycle, XeLL from Aurora).

Results go to `system.log` (`XenosGPUProbe: ...`). Results so far: `docs/gpu-memory-map.md`.

```sh
make -C XenosGPUProbe OSX_VERSION=tiger DARLING_SHELL= AS=/usr/bin/as CC=$HOME/osx360-build/bin/g5-cc \
  CXX=/usr/bin/powerpc-apple-darwin9-g++-4.2.1 LD=/usr/bin/ld_classic          # on the G5
# on the Xbox, from a copy of build_kext_tiger/XenosGPUProbe.kext:
sudo mkdir -p /Library/OSX360Dev && sudo cp -R XenosGPUProbe.kext /Library/OSX360Dev/
sudo chown -R root:wheel /Library/OSX360Dev/XenosGPUProbe.kext
sudo kextload /Library/OSX360Dev/XenosGPUProbe.kext
sudo kextunload -b fish.goldfish64.osx360.XenosGPUProbe
```
