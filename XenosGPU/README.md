# XenosGPU

The Xenos GPU driver of the GPU plan (`docs/gpu-driver-plan.md`, phase 1). During development it is loaded by hand from `/Library/OSX360Dev` with `tools/host/gpu-dev.sh XenosGPU`, on the `gpudev` boot profile (no XenonCoreGraphics); it is never loaded at boot and is not part of the package yet.

Step 1.1: on load it starts the command processor (reset, firmware, micro engine, `MEM_WRITE` markers), stops XenonPE's CPU framebuffer refresh and presents the desktop with the GPU every 15 ms (the linear framebuffer as a texture, resolved into the tiled scanout buffer). On unload it waits for the ring, halts the command processor and starts the CPU refresh again. It enables no GPU interrupt.

The ring, firmware and display sequences follow XenonCoreGraphics; the firmware and register headers are included from `XenonCoreGraphics/src`.
