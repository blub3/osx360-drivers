# Tiger GL stack and Quartz Extreme requirements (step 0.1)

Mac OS X 10.4.11 (8S165) on the Xbox 360 Slim, profile `main6`, 2026-11-25. Read from the installed binaries: copied off the Xbox over SSH, disassembled with `otool` on the G5, and annotated with `tools/gpu/annotate-ppc.py`. Apple's binaries are not in the repository.

**Result:** CoreGraphics turns the GL compositor (Quartz Extreme) on for a display only if the first renderer record for that display reports **at least 16 MB of video memory**. All Xenos GLDs so far report 0, because they copy the record from Apple's software renderer. That is a cause found in the code, not yet tested: step 4.2 is the first run that reports a VRAM size. The other conditions are already met, or are met by Apple's renderer the Xenos GLD forwards to.

## What is installed

- **OpenGL framework:** `GLEngine.bundle` (the GL front end), `GLDriver.bundle` (Apple's software renderer, renderer ID `0x0200`), `GLRendererFloat.bundle`, the `GLRaster*` bundles and `libGLProgrammability.dylib`.
- **Apple's hardware drivers in `/System/Library/Extensions`:**
  - GLDs and 2D plug-ins (`*GA.plugin`) for ATI Rage 128, Radeon, Radeon 8500 and Radeon 9700, and for GeForce 2MX, 3 and FX.
  - The matching kernel drivers, for example `ATIRadeon9700.kext`.
  - The Radeon 9700 (R300) set is the closest reference for Xenos.
- **Earlier Xenos experiments, still installed:**
  - GLDs: `GLDriverXenosNative`, `GLDriverXenosWSProbe`, `GLDriverXenosCompositor`, `GLDriverXenosCompositorRetry` (the one `main6` uses).
  - 2D plug-ins: `XenosLegacyBlit`, `XenosBeamBlit`, and ten `XenosWin*` copy-region plug-ins.
- **No `otool`, `nm` or `gdb` on the Xbox.** Use the G5 (Mac OS X 10.5.8).
- **`ioreg -l` and `ioreg -c <class> -l` fail** ("can't obtain properties") because of the NVRAM entry. A small IOKit property tool is needed for later steps.
- **Accelerator connections:** IOKit's statistics show 0 accelerator user clients (`IOAccelerationUserClient`, `XenosDisplaySurfaceClient`) and one framebuffer user client, so WindowServer has no accelerator connection open.
- **Log noise:** `system.log` gets a `XenosRing: rptr=… wptr=…` line about every 5 s from XenonCoreGraphics.

## GLD interface

- **Exports:** every GLD exports the same 63 functions: `gldGetRendererInfo`, `gldChoosePixelFormat`, `gldCreateContext`, `gldAttachDrawable`, `gldInitDispatch`, `gldUpdateDispatch`, the texture, buffer, vertex array, program, query, fence, framebuffer and memory-plug-in calls, `gldFlush` and `gldFinish`. This holds for Apple's software renderer, the ATI R300 driver and the NVIDIA FX driver alike; the list is in each binary's `nm -g` output.
- **How GLEngine calls them:**
  - It exports only 17 `gli*` functions (`gliQueryRendererInfo`, `gliChoosePixelFormat`, `gliCreateContext`, `gliAttachDrawable`, `gliSwapBuffers`, and so on).
  - It keeps a list of loaded GLDs (`glepGetPlugins`) and calls their functions through a table; `gldGetRendererInfo` is at offset `0x120`.
  - CoreGraphics calls the `gli*` functions through its own table (`cgls*` wrappers).
- **`gliQueryRendererInfo`:**
  - Calls each GLD's `gldGetRendererInfo(record, displayMask)` with a 56-byte (`0x38`) record.
  - Returns the records unchanged, except that it ORs `0x20000` into the renderer ID and `0x8000` into the flags.
  - A GLD that returns `0x2719` (bad display) is skipped.

### Renderer record (56 bytes, filled by `gldGetRendererInfo`)

| Offset | Field | Apple software renderer | ATI Radeon 9700 |
| --- | --- | --- | --- |
| `+0x00` | next (set by GLEngine) | 0 | 0 |
| `+0x04` | renderer ID | `0x200` | per-model ID ORed with the display index `<< 24` |
| `+0x08` | flags | `0x65d` | `0xa513` |
| `+0x0c` | buffer modes | `0xd` | `0xd` |
| `+0x10` | colour modes | from `glsGetRasterSpecs` | `0x8400` (ARGB8888, ARGB1555) |
| `+0x14` | accumulation modes | `0x00c0c000` | `0x00808000` |
| `+0x18` | depth modes | from `glsGetRasterSpecs` | `0x1c01` (0, 16, 24, 32 bit) |
| `+0x1c` | stencil modes | `0x81` | `0x80` |
| `+0x20`–`+0x28` | five 16-bit fields (`+0x24` is 4 for the software renderer) | 0 except `+0x24` | 0 |
| `+0x2a` | byte | 0 | 0 |
| `+0x2c` | word | 0 | 0 |
| `+0x30` | **video memory, MB** | **0** | from its kernel driver |
| `+0x34` | texture memory, MB | 0 | from its kernel driver |

- **Where ATI gets the memory sizes:** it opens its accelerator with `IOServiceOpen` type 1 and calls scalar method 3, which returns three values; two of them go into `+0x30` and `+0x34`.
- **Field names:** the buffer, colour, depth and stencil values match the public CGL mode bits (for example `0x8000` = ARGB8888, `0x4000` = RGB888, depth `0x1` = 0 bit).
- **Flag bits:** not decoded yet, except `0x1000` (below).

## When CoreGraphics enables the GL compositor

This is `CGXGLDisplayContextInitialize` in CoreGraphics, which runs inside WindowServer. The defaults come from `CoreGraphics.framework/Resources/Configuration.plist` → `GLCompositorConfiguration`:

| Key | Value on Tiger 10.4.11 |
| --- | --- |
| `GLCompositorMinimumVRAM` | 16 (MB) |
| `GLCompositorMinimumDisplayDepth` | 16 (code default 32) |
| `GLCompositorRequiredClasses` | `IOAGPDevice` |
| `GLCompositorUseOnPortable` | true |
| `tileWidth`, `tileHeight` | 256 |

Order of checks:

1. **For each display:**
   - `CGXAcceleratorForDisplayDevice` must find an accelerator (`IOAccelFindAccelerator` on the framebuffer's `IOAccelTypes`).
   - Its `AccelCaps` registry property is read.
   - **If no display has an accelerator with non-zero `AccelCaps`, the GL compositor is not used at all.**
   - Xenos: met. XenonCoreGraphics publishes `IOAccelTypes` and `IOAccelIndex` on XenonFramebufferTest and sets `AccelCaps` = 1.
2. **GL setup:** CoreGraphics (as root) initialises GL, then asks for a pixel format: Window (`80`), attributes 20, 21 and 22 = 8 (8-bit red, green, blue), depth size 0, stencil size 0.
3. **Renderers:** CoreGraphics queries the renderers for the display.
   - **The first record must report `+0x30` (video memory) ≥ `GLCompositorMinimumVRAM`.** Xenos: **not met**, 0 MB.
4. **Acceptance:** the renderer is accepted if either:
   - `isAccelUsable` is true **and** flag `0x1000` is set. `isAccelUsable` is true only with an `IOAGPDevice` among the accelerator's ancestors and no excluded instance. Xenos is an `IOPCIDevice`, so this path is closed.
   - **or** the display's `AccelCaps` is non-zero. Xenos: met. This is why the GPU branch sets `AccelCaps`.
5. **The returned pixel format must have:**
   - the display's bit in its display mask (`+0x30`);
   - `flags & 0x111` non-zero;
   - colour mode exactly `0x8000` or `0x4000`;
   - depth mode 1 and stencil mode 1 (0 bit);
   - the 16-bit field at `+0x22` = 0.

   This record is 52 bytes (`0x34`). Xenos: probably met, because the Xenos GLD forwards to Apple's renderer. Not checked.
6. **Display depth:** the display's depth must be ≥ `GLCompositorMinimumDisplayDepth`. Xenos: met, 32-bit desktop.
7. **Window acceleration:** `updateGLVRAMFlag` compares the display's GL VRAM (bytes) with the minimum (16 MB) again before it accelerates windows. `AllowClosedClamshellGLWindowAccel` concerns laptops only.

**Quartz 2D Extreme is separate** (`Quartz2DExtremeConfiguration`: at least 64 MB VRAM and 512 MB RAM). Its check also asks for `GL_APPLE_float_pixels` and `GL_ARB_fragment_program`. With 512 MB of RAM shared with the GPU, the Xbox will not reach the RAM minimum. That does not matter for Quartz Extreme.

## What this means for the plan

- **Step 4.2** (hybrid GLD) must report a video memory size of at least 16 MB in `+0x30`, and later the real heap size from `XenosGPU`, as ATI does through its kernel driver. It should keep everything else from Apple's renderer, so that steps 2 and 5 still pass.
- **First Quartz Extreme test:** a GLD that changes only `+0x30` is the smallest possible test of whether CoreGraphics then turns the compositor on. It needs only a WindowServer restart (R1), not a boot. Expected effect if it works: CoreGraphics draws the desktop through GL on Apple's software renderer, which may be slower than now until step 4.3 moves the work to the GPU.
- **Still unknown:**
  - which GL calls the compositor makes once it is on;
  - which renderer flags beyond `0x1000` matter.

  Steps 4.1 and 4.2 answer this with the logging GLD.
- **Reference for phase 3:** `ATIRadeon9700GA.plugin` is the reference for the 2D plug-in.

## How to repeat

```sh
# On the Xbox: pack the binaries (read-only)
ssh xbox 'cd / && tar cf - System/Library/Frameworks/ApplicationServices.framework/Versions/A/Frameworks/CoreGraphics.framework/Versions/A/CoreGraphics System/Library/Frameworks/OpenGL.framework/Versions/A/Resources/GLEngine.bundle System/Library/Frameworks/OpenGL.framework/Versions/A/Resources/GLDriver.bundle System/Library/Extensions/ATIRadeon9700GLDriver.bundle' > tiger-gl.tar
# On the G5: disassemble
otool -arch ppc -tV <binary> > out.dis
# On the PC: resolve the PIC-relative strings and CFStrings
python3 tools/gpu/annotate-ppc.py <binary> out.dis > out.ann
```

Functions to read in CoreGraphics:

- `CGXGLDisplayContextInitialize`
- `getAccelCapsBits`
- `isAccelUsable`
- `updateGLVRAMFlag`
- `CGXGLDisplayContextCapabilities`
- `CGXDumpGLInfo`

Functions to read in GLEngine and the GLDs:

- GLEngine: `gliQueryRendererInfo`
- each GLD: `gldGetRendererInfo`
