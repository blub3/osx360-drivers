# Unarmed WindowServer test downloads

The November 23, 2026 Tiger 10.4.11 cold boots verified two single-tile
moves on `gpuwinauto` and two eight-tile overlapping moves on
`gpuwinoverlap`. The overlapping moves matched fences 3–10 and 11–18.
Each matched 7,252 clipped pixels and 6,912 independent display pixels.
Quartz Extreme remained disabled. [Hardware tracker](https://github.com/blub3/osx360-drivers/blob/gpu/quartz-extreme/docs/gpu-milestones.md)

| Profile | OpenBIOS ELF | Restored mkext | Driver source tag |
| --- | --- | --- | --- |
| `gpuwinauto` | `WINAUTO.ELF` | `XWAU.MKE` | [gpu-0.0.5d48-gpuwinauto](https://github.com/blub3/osx360-drivers/tree/gpu-0.0.5d48-gpuwinauto) |
| `gpuwinoverlap` | `WINOVR.ELF` | `XWOV.MKE` | [gpu-0.0.5d48-gpuwinoverlap](https://github.com/blub3/osx360-drivers/tree/gpu-0.0.5d48-gpuwinoverlap) |

[Download the driver templates](https://github.com/blub3/osx360-drivers/releases/tag/gpu-0.0.5d48-gpuwinoverlap) · [Download the firmware ELFs and corresponding source](https://github.com/blub3/openbios/releases/tag/gpu-fw-windowserver)

## Restore the driver archive

The template contains the project portion of the tested mkext. Use your own
working `NativeHD.mkext` or another local mkext with the same encoded Tiger
IOGraphicsFamily 1.4.2 components. The tool reads that reference and
checks the SHA-256 of the resulting archive before writing an output.
The template extension identifies a file that must be restored before boot.

```sh
shasum -a 256 -c SHA256SUMS
python3 profile-mkext.py restore XWOV.MKE XWOV.MKE.template XWOV.MKE --reference /path/to/NativeHD.mkext
```

Use `XWAU.MKE` in all three positions for the single-tile profile. Both
restored archives matched their original USB images byte-for-byte in
testing. [Verified boot checksums](https://github.com/blub3/osx360-drivers/blob/gpu/quartz-extreme/docs/gpu-boot-checksums.txt)

## Build and run the Tiger test tools

Check out the source tag for the selected profile. Use your own PowerPC
toolchain and Tiger linker references. From the driver checkout, run
`gpu/lab/gpu-window-unarmed/build.sh /path/to/build --overlap`. Omit
`--overlap` for the single-tile profile. The download contains the bundle
metadata and shell runner for each test. Place the generated plug-in
binary in its bundle's `Contents/MacOS` directory. Place
`XenosWindowPluginCheck` and `XenosWindowRepaintCheck` next to the runner.
[Build dependencies](https://github.com/blub3/osx360-drivers/tree/gpu/quartz-extreme/gpu#rebuilding-a-profile) · [Profile build instructions](https://github.com/blub3/osx360-drivers/tree/gpu/quartz-extreme/gpu/lab/gpu-window-unarmed)

Copy the selected ELF, restored mkext and test directory to the XeLL boot
stick. Add its entry alongside the working menu entries and keep the
known-good firmware and menu. On Tiger, install the matching, locally
built plug-in as `root:wheel` under `/System/Library/Extensions`. After
cold boot, run the staged `RunXenosWindowUnarmed.sh --repaint` over SSH.

The download includes no Tiger system libraries, startup objects,
IOGraphicsFamily payloads or system images.
