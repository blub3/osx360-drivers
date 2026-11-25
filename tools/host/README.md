# Host-side test scripts

Scripts that run on the Linux host and test the Xbox 360 over SSH. They produced the results in `docs/platform-log.md` and `docs/audio-interrupt.md`.

## Requirements

- SSH alias `xbox` in `~/.ssh/config`, with key login and passwordless `sudo` on the Xbox for `dd if=/dev/kmem`, `xenon-cpu-start` and `xenon-audio-set`.
- `python3`, `md5sum`, `timeout` and OpenBSD `nc` on the host.
- On the Xbox, in the home folder: `xenon-cpu-start` (`tools/smp`) and, for the audio scripts, `xenon-audio-set` (`tools/audio`), both built on Tiger.

Logs and test data go to `$XBOX_TEST_OUT`, by default `~/xbox-test-logs`.

## Scripts

| Script | Purpose |
| --- | --- |
| `xbox-common.sh` | Shared helpers, sourced by the other scripts: interrupt counters from `per_proc`, desktop check, Ethernet check, CPU 1 stop/start, clock spread |
| `uptime2h.sh` | Two-hour uptime test with two CPUs: heartbeat every minute, load and Ethernet check every 10 minutes (`DURATION` in seconds) |
| `uptime3.sh` | The same with three CPUs |
| `smppark-test.sh [rounds]` | Stops and starts CPU 1 repeatedly (profiles `smppark`/`smppk2`) |
| `smp3-test.sh` | Stage 1b check: three CPUs, stopping and restarting CPUs 2 and 1 |
| `eth-measure.sh [cycle\|now] [MiB] [runs]` | Ethernet throughput unencrypted and over SSH, with CPU 1 running and stopped in the same boot |
| `audio-measure.sh` | Timer interrupt cost of XenonAudio, idle and while `say` plays |
| `audio-irq-test.sh` | Checks of the interrupt-driven audio build (profile `audirq`) |
| `audio-round.sh <label> <ack> <posfix> <interrupt>` | One listening round with the XenonAudio test switches |

Each script checks numeric pass criteria and logs `FAIL` lines and a summary.

## Boot file tools

| Tool | Purpose |
| --- | --- |
| `repack_platform.py SOURCE.mkext XenonPlatform.kext OUTPUT.mkext [stage1\|check]` | Replaces only XenonPlatform in an mkext and checks that every other entry is byte-identical |
| `repack_kext.py SOURCE.mkext BUNDLE_ID KEXT_DIR OUTPUT.mkext` | The same for any one kext (uses `repack_platform.py`) |

The profile ELF for XeLL is made from an mkext with `xbox360-profile-elf.py` from the `blub3/openbios` repository.

The register addresses in these scripts (`PerProcTable` at `0x365000`, `hwCtr` at `per_proc + 0x800`) are for the Tiger 10.4.11 kernel used in this project.
