<p align="center">
  <img src="docs/osx360.png" alt="OSX360" width="250">
</p>

# OSX360 drivers for Mac OS X Tiger

This fork of [osx360/osx360-drivers](https://github.com/osx360/osx360-drivers) extends the Xbox 360 kernel extensions from the original [OSX360 project](https://github.com/osx360). The fork focuses on Ethernet reliability, native HDMI mode switching and incremental Xenos GPU bring-up. Hardware results below are from an Xbox 360 Slim running PowerPC Mac OS X Tiger 10.4.11.

## What's working

| Area | Verified result |
| --- | --- |
| Ethernet | Fixed the transmit descriptor ring wrap marker for single- and multi-segment packets. The Slim transferred 72 MiB in one SSH session, answered 185 of 185 concurrent pings, and accepted a fresh SSH login. [Tested change](https://github.com/blub3/osx360-drivers/commit/87fd30a) |
| Native HDMI | Paired OpenBIOS and Tiger drivers switch the physical HDMI output between 720p and 1080p. A TV confirmed both timings, and iTunes audio was audible in both modes. [Tested change](https://github.com/blub3/osx360-drivers/commit/c4b62b4) |
| Xenos rendering | Hardware tests cover command-ring completion, offscreen drawing, texture sampling and filtering, alpha blending, depth, scissor and stencil, render-to-texture, and queued VBlank flips. |
| Tiger apps | Tests passed for reusable surfaces, shared drawables, concurrent client submissions, and process-local CGL/AGL rendering with pixel readback, buffer swaps, resizing and clipping. Tiger's stock CGL also discovered the native diagnostic GLD. |
| WindowServer | The `gpuwintile` diagnostic completed eight ordered WindowServer-owned GPU fences for a clipped 7,252-pixel repaint. The GPU, framebuffer and independent on-screen checks matched. Submission and readback took 1,086 ms, down from 4,276 ms in the 29-submission reference. |

**Quartz Extreme is still disabled.** The WindowServer result is a controlled repaint test. The remaining milestones are native WindowServer acceleration and an accelerated desktop smoke test. The tracker records the acceptance criteria, individual hardware runs and outstanding SMP checks. [GPU milestone tracker](https://github.com/blub3/osx360-drivers/blob/gpu/quartz-extreme/docs/gpu-milestones.md)

## Branches and boot profiles

- [`main`](https://github.com/blub3/osx360-drivers/tree/main) carries the tested Ethernet and native HDMI fixes.
- [`gpu/quartz-extreme`](https://github.com/blub3/osx360-drivers/tree/gpu/quartz-extreme) contains XenonCoreGraphics `0.0.5d48`, XenonFramebufferTest `0.0.5d3`, and the GPU test source, plug-ins and build recipes. The latest verified profile is [`gpu-0.0.5d48-gpuwintile`](https://github.com/blub3/osx360-drivers/tree/gpu-0.0.5d48-gpuwintile).

Each experimental boot entry pairs a specific OpenBIOS ELF with a driver archive. The profile index lists the pairing, component versions, source tags and recorded checksums. Keep the working XeLL entry and the 720p rollback when testing a new profile. [Boot profile index](https://github.com/blub3/osx360-drivers/blob/gpu/quartz-extreme/docs/gpu-boot-profiles.md) · [SHA-256 manifest](https://github.com/blub3/osx360-drivers/blob/gpu/quartz-extreme/docs/gpu-boot-checksums.txt)

## Build and boot

The boot chain is XeLL → OpenBIOS → Tiger on a console that can run XeLL. Start with the original project's installation guide and use the matching firmware and mkext from the branch or profile you intend to test. The companion firmware fork documents the verified Tiger USB SSD autoboot setup. [Upstream installation guide](https://github.com/osx360/osx360-drivers#installation) · [Upstream support matrix](https://github.com/osx360/osx360-drivers#support-status) · [Companion OpenBIOS fork](https://github.com/blub3/openbios)

The standard driver build uses Linux with Darling and the legacy Xcode toolchain. With the prerequisites installed, run the following from the repository root. [Driver build instructions](BUILDING.md)

```sh
make
make package
```

Historical GPU tests use their own PowerPC build, Tiger link and archive packaging recipes. Use the GPU work index for the required SDK, reference inputs and verification steps. [GPU test source and rebuild guide](https://github.com/blub3/osx360-drivers/tree/gpu/quartz-extreme/gpu)

## Credits and license

The original OSX360 port and driver foundation are the work of [Goldfish64](https://github.com/Goldfish64), [JoJo](https://github.com/buddyjojo) and the other OSX360 contributors. The upstream project credits [Apple](https://www.apple.com) for Mac OS X, [Lilu](https://github.com/acidanthera/Lilu) for the kernel patching and hooking basis, and [Free60](https://free60.org), [libxenon](https://github.com/Free60Project/libxenon), [freedreno](https://github.com/freedreno-zz/freedreno) and [Xenon Emulator](https://github.com/xenon-emu/xenon) for graphics and hardware references. [BSD 3-Clause license](LICENSE)
