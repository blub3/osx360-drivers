# Quartz Extreme milestone tracker
Updated 2026-11-23

Next Quartz Extreme checkpoint is milestone 7 of 8 — WindowServer
acceleration. Two integration milestones remain, including this one. For
each completed milestone, record its hardware check, mark it complete,
and report the next milestone and the remaining count.

## Previous milestones

- Platform groundwork — USB autoboot with rollback, Ethernet and SSH, the
  HDMI scaler test, and native HDMI switching between 720p and 1080p with audio.
- GPU fundamentals — command-ring pointers, completion marker, offscreen clear,
  solid triangle and retry, vertex-color interpolation, and a textured triangle.
- Offscreen composition — depth and alpha blending, indexed quad,
  render-to-texture, and overlapping texture composition.
- Presentation — moving textures on screen, double-buffered VBlank flips,
  the second-scanout-buffer retry, and queued triple-buffer flips.
- Tiger app submission — textured quad, app texture upload, vertex and UV upload,
  rotation, and two quads in one submission.
- Batched rendering — alpha blending, scissor, stencil and retry, combined
  scissor and stencil, depth with clipping, and two-pass render-to-texture.
- Filtering — bilinear and retry, mip levels 0 and 1, trilinear and the seed-67
  retry. Anisotropic filtering passed on hardware with XenonCoreGraphics
  0.0.5d34. Seed 67 checked 2,048 pixels with zero mismatches. Fence 1
  matched ticket 1. The trilinear control stayed gray across 72 pixels,
  and the anisotropic quad retained 36 dark and 36 light stripe pixels.

## Integration milestones

1. Reusable GPU surfaces — complete. Cold boot into gpusurface loaded
   XenonCoreGraphics 0.0.5d35. Two XenosSurface runs over SSH completed three
   clears each. All six fences matched their tickets, and each run checked
   3,072 pixels with zero mismatches. Both runs reused a handle for two
   clears, released it, recreated the surface and rejected stale and
   foreign handles and stale tickets.
2. Shared drawables — complete. Cold boot into gpushared loaded
   XenonCoreGraphics 0.0.5d36. XenosSharedDrawable passed over SSH.
   GPU fences 1 and 2 matched tickets 1 and 2. The owner and reader each
   verified 2,048 pixels across two clears with zero mismatches and matching
   checksums. The reader kept the same read-only 4 KiB pixel mapping and
   rejected a bad token and reuse of the consumed token.
3. Multiple-client queue and fences — complete. Cold boot into
   gpumulticlient loaded XenonCoreGraphics 0.0.5d37. XenosMultiClient
   passed over SSH with two independent Tiger processes. The paired jobs
   completed in FIFO order, tickets 2 then 3. Fences matched all four
   completed clears, and all 4,096 pixels matched. The driver cancelled
   queued ticket 4 when its owner exited. The survivor completed ticket 5.
   The live surface count returned from two to one and then to the
   starting count of zero, with no pending or active jobs. The local
   queue model also checked buffer retention across an in-flight exit.
4. Tiger renderer discovery — complete. On Tiger 10.4.11 with
   XenonCoreGraphics 0.0.5d37, the staged XenosCGLDiscovery test passed over
   SSH on the active 1920 × 1080 Xbox display. Public CGL enumeration returned
   Xenos renderer 0x0002e900 on display mask 0x00000001. The test matched the
   active CoreGraphics display to XenonFramebufferTest and XenosController
   on the same PCI device. Both stock renderer records were identical with
   the diagnostic plug-in loaded, and a fresh process returned the original
   two-renderer list. The plug-in is process-local and reports acceleration
   disabled. CGL returned zero pixel formats when asked to select it.
   Staged in OSX360BOOT/XenosCGLDiscoveryTest. Checksums matched for all 282
   pre-existing boot-volume files, including the native HDMI setup, prior GPU
   tests, and 720p backup.
5. CGL offscreen rendering — complete. Cold boot into gpucgloffscreen
   loaded XenonCoreGraphics 0.0.5d38 on Tiger 10.4.11. Over SSH,
   XenosCGLOffscreen selected the process-local Xenos renderer (0x0004e900),
   attached a 32 × 32 offscreen drawable and drew two textured quads with
   public OpenGL calls. GPU fences 1 and 2 matched tickets 1 and 2.
   glReadPixels verified 1,024 RGBA pixels per draw with zero mismatches,
   including the clear border, and both attached buffers matched. Full
   readback checksums were bd68d045 and 6ba3fca5. The second draw used a
   different texture, position and horizontal UV orientation.
   Staged on OSX360BOOT with all 286 pre-existing files verified before
   activation. The previous gpumulticlient config was saved as
   kboot.conf.before-gpucgloffscreen.bak.
6. Windowed drawables — complete. On Tiger 10.4.11 with
   XenonCoreGraphics 0.0.5d38, the staged XenosCGLWindow test opened a
   Carbon window and selected a double-buffered AGL drawable using the
   process-local Xenos renderer (0x0004e900). The SSH run drew four textured
   quads with GPU fences 15–18 matching their tickets. All 4,096 GPU back-buffer
   pixels and 52,224 display pixel comparisons matched. The front buffer
   retained the previous frame until the swap, and the drawable resized
   from 96 × 96 to 128 × 96 through aglUpdateContext. The AGL swap rectangle
   changed 3,600 pixels inside the clip and protected 192 pixels outside
   that would otherwise have changed. The GPU rendered to a fixed 32 × 32
   target, and Quartz presented the resolved pixels to the window.
   Staged in OSX360BOOT/XenosCGLWindowTest. All 293 pre-existing boot-volume
   files matched their pre-stage checksums. The existing boot was sufficient.
7. WindowServer acceleration — native GLD and display surface diagnostics
   passed, milestone open.
   The 1080p Tiger 10.4.11 baseline on XenonCoreGraphics 0.0.5d38 reported
   Quartz Extreme disabled. Native IOAccelFindAccelerator returned no
   matching accelerator. No IOGLBundleName was published. The system CGL
   query returned only its two software renderers. Eight real window moves
   and repaints left the Xenos fence at 19. An independent GPU client
   completed ticket and fence 20 with 1,024 matching pixels.
   The gpuxenosws boot profile adds XenonCoreGraphics 0.0.5d39 with test
   IOAccelerator association and per-client completion attribution. The
   staged XenosWindowServerTest verifies the native path independently from
   the app control. Its acceptance criteria require Quartz Extreme and
   fresh completed Xenos fences owned by the actual WindowServer PID.
   On the diagnostic cold boot, Tiger rejected the 0.0.5d39 kext because
   its boot linker could not resolve proc_selfpid. The desktop came up
   without XenonCoreGraphics. The diagnostic archive and profile were
   retained, and OSX360BOOT was restored to the verified gpucgloffscreen
   default using kboot.conf.before-gpuxenosws.bak. A subsequent cold boot
   loaded 0.0.5d38 and passed the offscreen CGL regression over SSH.
   GPU fences 1 and 2 matched their tickets and all 2,048 pixels matched.
   Tiger reported native HDMI switching available. The separate
   gpuxenoswsretry build (0.0.5d40) replaces the legacy com.apple.kernel.bsd
   dependency with com.apple.kpi.bsd 8.11.0, which exports proc_selfpid.
   Tiger's no-load kext linker reproduced the 0.0.5d39 undefined symbol
   and linked the exact 0.0.5d40 archive payload successfully. The retry
   is the default on OSX360BOOT. The cold boot loaded 0.0.5d40 at 1080p.
   Tiger's IOAccelFindAccelerator returned success and found
   XenosDisplayAccelerator at index 0. Two independent GPU client runs
   verified fence ownership for PIDs 236 and 268. Fences 1 and 2 matched
   their tickets, and all 2,048 readback pixels matched.
   The working gpucgloffscreen profile and its exact configuration are
   available via kboot.conf.before-gpuxenoswsretry.bak. All 303 existing
   files matched their pre-stage checksums. The rollback, native HDMI,
   and 720p backup checksums also passed after the cold boot.
   Quartz Extreme stayed disabled and eight window moves and repaints
   produced zero WindowServer-owned GPU fences.

   Native Xenos GLD diagnostic — the staged test passed over SSH on
   0.0.5d40. Tiger's stock CGL and GLEngine discovered renderer 0x0002e900
   on the Xbox display using a temporary per-process resource directory.
   The GLD renderer-info callback submitted a textured quad. On the
   staged run, ticket and fence 4 matched, the kernel recorded the probe
   as owner (PID 479), and all 1,024 pixels matched (checksum 5765efc5).
   Both stock renderer records matched before, during, and after the run.
   The separate gpunativegld candidate stages 0.0.5d41 with IOGLBundleName
   and the Tiger GLD bundle. Both candidate kexts passed Tiger's offline
   linker. The active gpuxenoswsretry profile and all 310 pre-existing
   files matched their checksums after staging, including HDMI, the
   earlier tests, and the 720p backup.
   The GLD bundle was installed on Tiger as root:wheel and verified
   against the staged files. gpunativegld is active on OSX360BOOT, with
   gpuxenoswsretry saved as kboot.conf.before-gpunativegld.bak. All 317
   protected files matched their checksums during activation.
   After the cold boot, SSH confirmed 0.0.5d41 and IOGLBundleName set to
   GLDriverXenosNative. Tiger's stock CGL found Xenos renderer 0x0002e900
   using its default resource paths. Both stock software renderers were
   still listed. The two GLD callbacks completed fences 1 and 2, matching
   their tickets and owning PIDs 247 and 256. Both 1,024-pixel readbacks
   had zero mismatches and checksum 5765efc5. Post-boot checksum checks
   passed for the installed bundle, rollback profile, native HDMI kext,
   and 720p backup. The diagnostic renderer reports accelerated=0.
   The WindowServer display surface diagnostic is staged as
   gpudisplaysurface (XenonCoreGraphics 0.0.5d42). The G5 link and
   Tiger's offline kext link both passed. The probe uses the Xbox
   framebuffer association and Tiger's native IOAccel surface calls
   to create, shape, flush, read-lock, unlock, and destroy a diagnostic
   offscreen surface. It checks two GPU clears, matching fence owners
   and all 1,024 pixels per clear.
   On the active 0.0.5d41 boot, the native surface open returned
   unsupported as expected. The existing XenosSurface control completed
   fences 3, 4, and 5 and verified all 3,072 pixels. All 319 pre-existing
   files matched their checksums after staging. gpudisplaysurface is now
   the boot default, and the working gpunativegld configuration is saved
   as kboot.conf.before-gpudisplaysurface.bak. All 324 protected files
   matched their checksums during activation, including HDMI, earlier
   tests, and the 720p backup.
   After the cold boot, SSH confirmed 0.0.5d42 and IOAccelFindAccelerator
   succeeded at framebuffer index 0. Tickets and GPU fences 1 and 2
   matched, both owned by probe PID 247. Each 1,024-pixel readback matched
   its clear color, ff173ba2 and ffb21163 respectively, with a 128-byte
   stride and zero mismatches. The probe passed the create, shape, flush,
   read-lock, unlock, and destroy lifecycle. The driver logged readback
   checksums 3d4ed5c5 and 69e21dc5. The rollback and HDMI checksums passed
   after boot, and Tiger reported native HDMI switching available.
   The probe verified an offscreen surface owned by the SSH test process.
   The next build, gpuwssubmit (0.0.5d43), is staged on OSX360BOOT with
   the separate GLDriverXenosWSProbe bundle. Its diagnostic capability
   flag lets Tiger enter GLD initialization on the Xbox PCI device. The
   GLD is designed to run two native offscreen surface clears during
   WindowServer initialization. The kernel records both tickets, fences,
   readback checksums, and the submitting PID. An SSH check compares the
   recorded owner with the running WindowServer PID. The Quartz Extreme
   check requires an accelerated Xenos renderer and fresh WindowServer
   fences during window repaints. It excludes the startup fences.
   Both candidate kexts passed Tiger's offline linker. The staged
   verifier ran over SSH on the active 0.0.5d42 boot. Quartz Extreme
   stayed off across eight window moves and repaints, with zero
   WindowServer fences. The app control passed fence 4 and matched all
   1,024 pixels. The startup check rejected that app-owned completion.
   A live smoke test loaded the candidate GLD and confirmed its
   initializer left the GPU fence unchanged outside WindowServer.
   All 326 pre-existing files matched their checksums after staging,
   including the active gpudisplaysurface profile, HDMI and 720p backup.
   GLDriverXenosWSProbe was installed on Tiger as root:wheel and verified
   against the staged bundle. gpuwssubmit is the boot default, with the
   working gpudisplaysurface saved as kboot.conf.before-gpuwssubmit.bak.
   All 333 protected files matched their checksums during activation.
   Cold boots on November 22 and 23 both loaded XenonCoreGraphics
   0.0.5d43. On the November 23 boot, Tiger found the Xenos accelerator
   and its GLD at framebuffer index 0. CGL listed renderer 0x0002e900
   with accelerated=0. WindowServer PID 72 produced no startup fence
   records and no GPU fences during eight window moves and repaints.
   CGDisplayUsesOpenGLAcceleration returned false before and after.
   On the same boot, the native surface control passed fences 1 and 2
   with two matching 1,024-pixel readbacks, owned by probe PID 295. The
   independent quad control passed fence 3 with 1,024 matching pixels,
   owned by PID 296. Both installed GLDs, the rollback configuration,
   native HDMI and 720p backup passed post-boot checksum checks.
   A privileged memory-map check showed neither GLEngine nor the startup
   GLD mapped into WindowServer PID 72. In an isolated Tiger process the
   same stock cglsInitialize call loaded both binaries using the Xenos
   accelerator, so the installed GLD and its loader were functional.
   The 0.0.5d43 startup probe depended on WindowServer entering that
   optional GL path. No startup callback was observed on either boot.
   The separate gpuwsfbretry profile stages XenonCoreGraphics 0.0.5d44
   and XenonFramebufferTest 0.0.5d2. A successful native framebuffer-open
   request from WindowServer now triggers two diagnostic offscreen GPU
   clears. The kernel captures the caller's PID at the request, queues the
   work on the GPU work loop, and records both hardware fences and pixel
   checksums. The verifier requires the open, queue and both completions
   to match the live WindowServer PID. It checks repaint fences and
   Quartz Extreme independently. The retry uses the already installed
   GLDriverXenosNative bundle.
   Both retry kexts passed Tiger's offline linker. Over SSH on 0.0.5d43,
   the staged verifier found no framebuffer-triggered records, as
   expected. Its app control completed fence 4 with 1,024 matching
   pixels. All 335 pre-existing files matched their checksums after
   staging. gpuwsfbretry is now the boot default. The previous gpuwssubmit
   configuration is saved as kboot.conf.before-gpuwsfbretry.bak. All 340
   protected files matched their checksums during activation.
   After the cold boot, SSH confirmed XenonCoreGraphics 0.0.5d44 and
   XenonFramebufferTest 0.0.5d2. The framebuffer-open PID, queue PID,
   and live WindowServer PID all matched at 72. Both diagnostic GPU
   fences, 1 and 2, matched their tickets and recorded owner PID 72.
   Each offscreen clear matched all 1,024 pixels. The readback checksums
   were 3d4ed5c5 and 69e21dc5. Both GLDs, the rollback configuration,
   native HDMI and the 720p backup passed post-boot checksum checks.
   Tiger reported Quartz Extreme disabled. The next checkpoint is a
   GLD compositor context and GPU fences driven by window repaints.
   The gpucompositor diagnostic is staged on OSX360BOOT with
   XenonCoreGraphics 0.0.5d45, XenonFramebufferTest 0.0.5d2 and
   GLDriverXenosCompositor. Both kexts passed Tiger's offline linker.
   All 342 pre-existing files matched their checksums after staging,
   including the active gpuwsfbretry profile, native HDMI and 720p backup.
   The staged private CGL test selected renderer 0x0002ea00. Both GPU
   fences, 7 and 8, matched their tickets and owner PID 684, with 1,024
   matching GPU pixels each. OpenGL also read back the expected red and
   green drawable pixels. On the active 0.0.5d44 boot, eight window moves
   and repaints produced zero fresh WindowServer-owned fences. Quartz
   Extreme was off before and after. The new GLD submits an offscreen
   GPU probe on a successful compositor drawable attachment and uses
   Tiger's software rasterizer for desktop pixels. The next check needs
   the staged GLD installed and a cold boot into gpucompositor to see
   whether WindowServer attaches a drawable and owns fresh repaint
   fences. Quartz Extreme and actual GPU desktop composition remain open.
   GLDriverXenosCompositor was installed on Tiger with matching
   checksums and root:wheel ownership. gpucompositor is the boot default.
   kboot.conf.before-gpucompositor.bak preserves the gpuwsfbretry default.
   All 351 protected files matched their checksums during activation.
   The November 23 cold boot loaded XenonCoreGraphics 0.0.5d45 and
   XenonFramebufferTest 0.0.5d2. Tiger found the Xenos accelerator at
   index 0 and reported GLDriverXenosCompositor as the framebuffer's
   GLD name. WindowServer PID 76 owned framebuffer startup fences 1
   and 2, each with 1,024 matching GPU pixels. Eight window repaints
   produced zero fresh WindowServer-owned fences, and Quartz Extreme
   remained off. The logs contained no compositor GLD initialization
   or repaint callback from WindowServer on this boot. Default CGL
   enumeration from SSH listed two unaccelerated software renderers.
   The same-boot private CGL control selected 0x0002ea00 and passed
   fences 3 and 4, both owned by test PID 337, with 1,024 matching GPU
   pixels per pass and correct red and green drawable readbacks.
   A read-only inspection of WindowServer PID 76 found 256-by-256
   compositor tiles, no CGLS plugin, and no compositor contexts. Its
   memory map contained neither GLEngine nor GLDriverXenosCompositor.
   The installed Tiger CoreGraphics binary gates ordinary repaint GL
   access on a working framebuffer blit engine and surface support.
   On the live Xenos framebuffer, IOAccelFindAccelerator passed at
   index 0, IOPSAllocateBlitEngine returned 0xe00002c7 with no engine,
   and IOAccelRevision was absent.
   A transition request for a private window independently reached
   CGLS. Tiger logged AccelCaps 00000001. WindowServer PID 76 loaded the
   GLD and received a valid pixel format, then returned transition status
   1004 before creating a context. The software delegate reported zero
   renderer video memory in the retry control. The stock compositor
   checks a 16 MiB minimum before creating a context.
   The separate gpucompretry profile is staged on OSX360BOOT. It uses a
   uniquely named GLD and keeps the 0.0.5d45 and 0.0.5d2 kext binaries.
   Both kexts passed Tiger's offline linker. All 353 existing files
   matched their checksums, including active gpucompositor, its rollback,
   native HDMI, earlier GPU tests and the 720p backup.
   The staged private format control passed fence 10 with 1,024 matching
   pixels and confirmed one submission across two format requests. The
   private offscreen drawable control passed fences 11 and 12, each with
   1,024 matching GPU pixels and the expected OpenGL readback. Eight
   ordinary repaints on the active profile again produced no fresh
   WindowServer fences and Quartz Extreme stayed off.
   The retry's opt-in transition test checks a fresh WindowServer-owned
   fence at GLD pixel-format selection. The named build still needs a
   cold boot for that check. Native repaint attachment requires the
   framebuffer blit and surface path. Quartz Extreme remains open.
   The retry GLD was installed on Tiger with matching checksums and
   root:wheel ownership. gpucompretry is the boot default.
   kboot.conf.before-gpucompretry.bak preserves gpucompositor.
   All 364 protected files matched their checksums during activation.
   The retry cold boot stalled at a pixelated boot screen and SSH did
   not return. The automatic transition verifier was stopped before it
   armed the GPU or requested a transition. There is no cold-boot result
   for the retry fence or Quartz Extreme. Restored gpucompositor on
   the mounted USB from the checksum-verified rollback configuration.
   The retry files, native HDMI setup, previous rollback and 720p backup
   passed their checksum checks after the restore.
   SSH confirmed a fresh recovery boot into gpucompositor with
   XenonCoreGraphics 0.0.5d45, XenonFramebufferTest 0.0.5d2, WindowServer
   PID 66, Finder and Dock. The restored boot and both rollback files
   matched, along with native HDMI and the 720p backup. On the recovery
   boot, Quartz Extreme remained off during eight window repaints and
   the GPU fence remained at 2. The retry GLD is still installed as a
   separate bundle and the retry profile is still on the stick. The next
   checkpoint is identifying where the retry boot stalled before trying
   its WindowServer transition test again.
   The startup photo (IMG_5501.jpeg) shows heavy block corruption
   after XeLL, with no readable OpenBIOS or Apple startup text. The
   recovered system's boot history has no Tiger startup or
   WindowServer entry for the failed attempt. The observed interval
   covers the handoff after XeLL and early startup before Tiger
   logging. The photo cannot identify whether scanout corruption was
   the cause or obscured another failure. Tiger's mkextunpack opened
   both the working and failed archives and listed all nine kexts.
   Separately staged gpucompbootfix with short FAT filenames
   CMPFIX.ELF and XWCF.MKE. The OpenBIOS image differs from the
   working gpucompositor image only in the mkext filename. The
   archive retains the working length of 389,568 bytes and all
   payload offsets, with the two retry GLD selectors carried in
   their original slots. Tiger unpacked all nine executables and
   matched them byte for byte to the working archive. Both graphics
   kexts passed the offline linker. The compact alternate config
   retains all 47 earlier entries and uses the installed retry GLD.
   All 388 pre-existing files matched after staging. The active
   kboot.conf still selects gpucompositor. The next checkpoint is a
   stable Tiger desktop and SSH on gpucompbootfix, followed by the
   WindowServer transition fence check.
   gpucompbootfix is active on OSX360BOOT.
   kboot.conf.before-gpucompbootfix.bak matches the working
   gpucompositor configuration. The installed retry GLD, boot
   files, native HDMI files and 720p backup passed checksum
   checks. The November 23 cold boot reached Tiger with the USB
   mounted, XenonCoreGraphics 0.0.5d45 and XenonFramebufferTest 0.0.5d2.
   WindowServer (PID 64), Finder and Dock were stable across two SSH
   checks. The framebuffer selected GLDriverXenosCompositorRetry.
   The transition request initialized the GLD and selected a pixel
   format. The GLD's transition-format callback submitted GPU ticket
   3, and the kernel recorded matching fence 3, owner PID 64, 1,024
   matching pixels, zero mismatches and checksum 966708c5. Startup
   fences 1 and 2 were recorded separately. The transition API
   returned 1004 without an animation handle. Quartz Extreme stayed
   off before and after eight ordinary repaints, which produced no
   new WindowServer fences. gpucompbootfix remains active. The next
   milestone 7 checkpoint is supporting Tiger's framebuffer blit
   and surface interfaces so ordinary repaints can exercise the GLD
   drawable callback.
   The gpufbblit profile is staged on OSX360BOOT. It adds a Tiger
   IOGraphicsAccelerator plug-in registration to the framebuffer
   personality and retains every kext binary, all archive payload
   offsets, the file length and all 48 earlier XeLL entries. Tiger
   unpacked all nine kexts and passed offline linking for both graphics
   kexts. All 393 pre-existing files matched their checksums after
   staging, including the active gpucompbootfix configuration, its
   gpucompositor rollback, native HDMI, earlier tests and the 720p backup.
   The staged private plug-in registered fill, copy and region-copy
   callbacks. A controlled fill on Tiger passed ticket and fence 5
   (test PID 611) and read back 1,024 matching pixels with zero
   mismatches. A malformed fill submitted no GPU work and a repeated
   fill submitted no second fence. The original desktop blit uses
   Tiger's software fallback.
   Current-boot baseline at 1920×1080: stock IOPS allocation returned
   0xe00002c7 without an engine. Eight window repaints left fence 5
   unchanged, with no WindowServer fence and Quartz Extreme off.
   The cold-boot test was designed to verify stock IOPS allocation
   and a fresh WindowServer-owned fence and pixel readback from a
   normal repaint.
   Quartz Extreme remains open. The test's fenced clear targets a
   diagnostic surface. Testing accelerated desktop composition follows
   once the callback path is verified.
   XenosLegacyBlit.plugin is installed on Tiger. Both files matched
   the staged checksums and have root:wheel ownership. gpufbblit was
   selected for the cold-boot test. kboot.conf.before-gpufbblit.bak preserves the verified
   gpucompbootfix configuration, alongside the earlier gpucompositor
   rollback. All 401 protected files matched across activation.
   gpufbblit eventually reached Tiger 10.4.11 over SSH. The staged
   framebuffer registration was live, and WindowServer started the
   installed plug-in. WindowServer then repeatedly crashed during the
   startup display fade. The backtrace identified CGXBlendWithColor.
   The return address and installed Tiger binaries pinpoint the null
   dispatch at IOFBBeamPosition, which calls the plug-in vtable's
   +0x38 beam-position slot. This test left that slot empty. The crash
   occurs before an ordinary repaint can be tested.
   The surviving startup logs show both framebuffer-open diagnostic
   fences (for example, 109 and 110 under WindowServer PID 432).
   Each checked 1,024 pixels with zero mismatches. These are startup
   results. No repaint fence, repaint pixel readback or Quartz Extreme
   status was collected from gpufbblit.
   Mounted OSX360BOOT over SSH and restored gpucompbootfix from the
   verified kboot.conf.before-gpufbblit.bak. The older rollback, HDMI
   and 720p hashes still match. The failed gpufbblit profile and all
   its staged files remain available. The working profile does not
   register the installed blit plug-in. SSH confirmed the recovery
   cold boot with WindowServer PID 78, Finder and Dock, plus the
   working framebuffer and graphics kexts. Rechecked both rollback
   configurations, the native HDMI files, the 720p backup, and the
   staged gpufbblit files after recovery.
   The separate gpufbblitbeam profile is staged on OSX360BOOT.
   XenosBeamBlit.plugin supplies the beam-position callback and returns
   kIOReturnUnsupported for scanline timing. Tiger branches to its
   immediate display-transfer path on that result. Synchronization
   and legacy surface operations also return unsupported safely.
   Tiger's stock IOFBBeamPosition and IOFBSynchronize wrappers passed
   on the private plug-in with no GPU work. The staged copy then
   passed GPU fence 4 (test PID 525), with all 1,024 pixels matching
   and zero mismatches. The active 1080p desktop baseline completed
   eight repaints with no new fence and Quartz Extreme off.
   Tiger unpacked all nine kext binaries byte for byte and validated
   both graphics kexts with its offline linker. The mkext retains its
   length and payload offsets, and the alternate XeLL configuration
   keeps all 49 earlier entries, including gpufbblit. All 403 existing
   files matched after staging. gpucompbootfix is still active.
   Cold-boot target: verify the startup fade, a WindowServer repaint
   fence and pixel readback, and Quartz Extreme status on gpufbblitbeam.
   XenosBeamBlit.plugin was installed with matching checksums and
   root:wheel ownership. gpufbblitbeam was selected for the cold-boot
   test. kboot.conf.before-gpufbblitbeam.bak preserves gpucompbootfix.
   All 411 protected files matched across activation.
   The gpufbblitbeam cold boot reached a stable 1920×1080 Tiger
   desktop. WindowServer PID 70 logged the startup beam-position
   fallback (0xe00002c7) and remained running through the repaint test.
   Tiger's stock IOPSAllocateBlitEngine opened the retry plug-in
   with score 100. During eight overlapping window repaints and
   moves, its region-copy callback submitted GPU ticket 3 from
   WindowServer. Fence 3 matched, with 1,024 matching readback pixels
   and zero mismatches. Framebuffer startup fences 1 and 2 were
   recorded separately. Quartz Extreme remained off before and after
   the run, and default CGL still listed two software renderers.
   The retry remains active. SSH verified both rollback generations,
   native HDMI, earlier blit files and the 720p backup after the boot.
   This completes the ordinary-repaint submission checkpoint within
   milestone 7. The remaining milestone 7 work is connecting this
   callback path to accelerated WindowServer composition.
   The separate gpuwinrepaint profile is staged. It targets a single
   opaque 16×16 region during the move of a four-color test window.
   For a matching Tiger blit the plug-in stages the source pixels,
   renders them as a texture on Xenos, verifies the GPU result and
   copies that result to the screen destination. The CPU handles
   upload and writeback. The plug-in returns success only for the full
   matched rectangle. Other operations keep Tiger's software path.
   The private plug-in passed the beam-position, synchronization and
   surface fallback checks. The staged GPU control passed fence 6,
   with all 256 copied pixels and 3,840 guard pixels matching. On the
   active gpufbblitbeam boot, the same window moved from (224,208) to
   (304,248) and all 256 on-screen pixels matched the source. The
   framebuffer alias matched the CoreGraphics readback, no new fence
   was submitted by that baseline, and Quartz Extreme remained off.
   Tiger unpacked all nine kext binaries byte for byte and passed its
   offline linker checks for both graphics kexts. All 50 earlier XeLL
   entries and all 412 pre-existing files matched after staging,
   including the active configuration, rollback, HDMI and 720p backup.
   Cold-boot target: verify a matching WindowServer-owned GPU fence,
   callback and visible destination pixels from the same window move.
   XenosWinBlit.plugin was installed with matching checksums and
   root:wheel ownership. gpuwinrepaint was selected for the cold-boot
   test. kboot.conf.before-gpuwinrepaint.bak preserves gpufbblitbeam.
   All 420 protected files matched across activation.
   The gpuwinrepaint cold boot reached a stable Tiger desktop.
   WindowServer PID 75 loaded XenosWinBlit and called its beam-position
   fallback. Tiger's stock IOPSAllocateBlitEngine passed with score 100.
   Both live trials moved the window and matched all 256 on-screen
   pixels, but the driver remained at framebuffer startup fence 2.
   No matching XenosWinBlit copy was recorded and no fresh repaint
   fence was observed. Quartz Extreme stayed off.
   The same-boot private control passed fence 3 with 256 copied pixels
   and 3,840 guard pixels matching. Its fence belongs to the control
   process. The desktop, WindowServer PID and test cleanup were
   verified over SSH after the recheck. gpuwinrepaint remains active.
   A live Tiger kernel trace of the original 16×16 test showed
   WindowServer PID 75 entering both copy-region and rectangle-copy.
   Both opened repaint-window.arm successfully. The driver fence
   stayed unchanged, with no log from the GPU copy path. Disassembly
   showed that the original build returned silently unless the
   callback supplied one exact 16×16 rectangle at the armed origins.
   The syscall trace did not expose the callback's exact geometry or
   which pre-submission check rejected it.
   The separate gpuwinretry build accepts a bounded list of clipped
   rectangles sharing one translation and covering the armed tile.
   It snapshots the requested pixels to handle overlaps, uses the GPU
   output for the 16×16 tile and copies the rest from the snapshot.
   It records callback geometry and rejection reasons while armed.
   The staged private control passed fence 6, matching all 256 GPU
   tile pixels, all 392 pixels in the overlapping clipped copy, and
   all 3,704 guard pixels. Both window workloads matched all 256
   on-screen pixels in baseline mode with Quartz Extreme off.
   Tiger unpacked all nine unchanged kext executables and passed the
   offline linker checks for both graphics kexts. The alternate
   config retains all 51 earlier boot entries. All 423 pre-existing
   files matched after staging, including the active gpuwinrepaint
   profile, rollbacks, native HDMI, earlier tests and 720p backup.
   Cold-boot target: check the callback geometry, WindowServer
   fence and visible pixels during the original or overlapping move.
   XenosWinFix.plugin was installed with matching checksums and
   root:wheel ownership. gpuwinretry was selected for the cold-boot
   test. kboot.conf.before-gpuwinretry.bak preserves gpuwinrepaint.
   All 431 protected files matched across activation.
   gpuwinretry reached a stable Tiger desktop. WindowServer PID 75
   loaded XenosWinFix, used the startup beam fallback, and stayed up.
   Stock IOPS allocation passed with score 100. Both the region-copy
   and rectangle-copy callbacks accepted the geometry and translation
   of each move. Tiger supplied one 324-pixel copy for the small
   window, which exceeded the original exact 256-pixel requirement.
   It supplied one 7,252-pixel copy for the overlapping window.
   All four callbacks reported framebuffer-map before GPU submission.
   The logs do not identify which individual mapping check failed.
   The live fence stayed at startup fence 2. Both moves showed all
   256 expected screen pixels through the software path. Quartz
   Extreme stayed off. The same-boot private copy passed fence 3
   under test PID 356, with 256 matching tile pixels, 392 copied
   region pixels and 3,704 intact guard pixels. The retry remains
   active and the gpuwinrepaint rollback was rechecked after boot.
   A read-only Tiger probe reproduced the failure by running the
   same mapping calls as the console account and as WindowServer's
   account. Both opened the shared framebuffer connection. The
   console account mapped the 8 MiB aperture read/write, while the
   WindowServer account received 0xe00002c2 on VRAM type 110.
   Tiger's shared VRAM mapping checks the 'local' privilege before
   granting that descriptor.
   The separate gpuwinmap profile is staged with XenonFramebufferTest
   0.0.5d3 and XenosWinMap.plugin. The framebuffer driver offers a
   dedicated mapping of the existing aperture to the exact task that
   opened the stock server connection. Task identity is rechecked at
   mapping. The plug-in reports the failed map phase and status.
   Tiger unpacked the revised framebuffer kext and confirmed eight
   other kext executables were byte-identical. Both graphics kexts
   passed offline linking. All payload offsets were preserved, with
   the last framebuffer payload extending the archive by 5,120 bytes.
   The staged private GPU copy passed fence 4 with 256 matching tile
   pixels, 392 matching region pixels and 3,704 intact guards. The
   previous kext rejected the private selector as expected. Both
   normal console baselines passed with Quartz Extreme off.
   All 433 pre-existing files matched after staging. gpuwinretry is
   still active, and its rollbacks, native HDMI, earlier tests and
   720p backup are preserved. The cold-boot target is owner-only
   mapping, a GPU repaint fence, and the visible result.
   XenosWinMap.plugin was installed with matching checksums and
   root:wheel ownership. gpuwinmap was selected for the cold-boot
   test. kboot.conf.before-gpuwinmap.bak preserves gpuwinretry.
   All 441 protected files matched across activation.
   gpuwinmap cold-booted to a stable Tiger desktop with
   XenonFramebufferTest 0.0.5d3, XenonCoreGraphics 0.0.5d45 and
   WindowServer PID 76. The separate console process was denied
   the private mapping with 0xe00002c1. WindowServer mapped the
   aperture successfully (0x00000000) and its region-copy callback
   completed GPU ticket and fence 3 during the 324-pixel window move.
   The GPU readback had zero mismatches, and all 256 test-tile pixels
   matched on the display. Startup fences 1 and 2 were recorded
   separately. WindowServer, Finder and Dock stayed up after the
   repaint. gpuwinmap remains active and the older rollback, native
   HDMI, earlier tests and 720p backup passed the post-boot checks.
   The on-screen WindowServer GPU tile-copy checkpoint passed.
   Quartz Extreme stayed off, so milestone 7 remains open.
   The gpuwinfull test is staged on OSX360BOOT with XenosWinAll.plugin,
   WINFULL.ELF and XWFR.MKE. The armed callback checks the entire clipped
   region up to 512 unique pixels across two GPU submissions. It
   verifies the GPU results before transferring them to scanout and
   reads every destination pixel. Larger moves fall back to Tiger's
   software copy. The previous small live window move covered 324 pixels.
   The staged private control passed over SSH with ordered fences 6
   and 7. All 392 clipped pixels matched in both GPU and destination
   readbacks, with all 3,704 guard pixels intact. A separate console
   task was denied WindowServer's private framebuffer mapping, as
   expected. The baseline window move matched all 256 visible test
   pixels with Quartz Extreme off.
   Tiger unpacked all nine byte-identical kext executables and
   passed both graphics kext offline linker checks. All 443 existing
   USB files matched after staging, including the active gpuwinmap
   profile, gpuwinretry rollback, native HDMI, earlier tests and
   720p backup. The alternate config retains all 53 earlier boot
   entries. XenosWinAll.plugin is now installed on Tiger. Its checksums
   and root:wheel ownership passed verification. gpuwinfull is selected
   for the cold boot. kboot.conf.before-gpuwinfull.bak preserves gpuwinmap,
   and all 451 protected files matched across activation.
   gpuwinfull cold-booted to a stable Tiger desktop with
   XenonFramebufferTest 0.0.5d3 and XenonCoreGraphics 0.0.5d45.
   WindowServer PID 75 loaded XenosWinAll and completed GPU fences
   3 and 4 in order during one 324-pixel clipped copy. Fences 1 and 2
   were from framebuffer startup. All 324 pixels matched in both
   the GPU and scanout readbacks (region hash b9d0a0d5). The separate
   on-screen check matched all 256 test-tile pixels, and the GPU
   reported zero mismatches. The non-owner aperture check passed.
   The full clipped on-screen copy checkpoint passed. Quartz Extreme
   remained off before and after the move, so milestone 7 stays open.
   gpuwinfull remains active. Its gpuwinmap rollback, native HDMI
   and 720p backup passed the post-boot checksum checks.
   The separate gpuwinbatch profile is staged on OSX360BOOT with
   XenosWinBatch.plugin, WINBATCH.ELF and XWBT.MKE. It targets the
   overlapping 96 × 72 window move that previously produced a
   7,252-pixel clip. The callback snapshots the source, processes
   the complete clip in 256-pixel GPU batches, and publishes only
   after every ticket, fence and GPU pixel matches. The CPU transfers
   the verified RGB results to scanout. The diagnostic is capped at
   8,192 pixels and one armed attempt per WindowServer.
   The staged offscreen control passed over SSH. Fences 34 through 62
   completed in order across all 29 batches, including the partial
   final batch. All 7,252 pixels matched in both readbacks, all 9,132
   guard pixels stayed intact, and GPU submissions took 4.28 seconds
   with the current cooldown. The unarmed on-screen baseline matched
   6,912 of 6,912 window content pixels with Quartz Extreme off.
   Tiger unpacked all nine byte-identical kext executables, and both
   graphics kexts passed offline linking. The alternate config
   retains all 54 earlier boot entries. All 453 pre-existing files
   matched across staging, including gpuwinfull, the gpuwinmap
   rollback, native HDMI, earlier tests and the 720p backup.
   XenosWinBatch.plugin is installed with matching checksums and
   root:wheel ownership. gpuwinbatch is selected for the cold boot.
   kboot.conf.before-gpuwinbatch.bak preserves gpuwinfull. All 461
   protected files matched across activation. On the first cold boot,
   WindowServer PID 75 completed all 29 fences, numbered 3 through 31,
   in order. All 7,252 clipped pixels matched in both the GPU and
   framebuffer readbacks (region hash eeb35d15). The GPU work took
   4.28 seconds. The separate on-screen checker stopped early, while
   the GPU was still processing fence 10.
   The checker now waits for the asynchronous move for up to 12
   seconds. Its unarmed baseline matched all 6,912 window content
   pixels. Only the checker changed on the stick, and the other 462
   files matched. On the second cold boot, the GPU startup marker
   failed, the ring read pointer stopped at 0x000003df, and the first
   startup clear returned no fence. The repaint used Tiger's software
   fallback and all 6,912 on-screen content pixels matched. The
   SSH verifier now checks both startup clears before arming.
   The third cold boot was reachable over SSH and had WindowServer,
   Finder and Dock running. A 1920 × 1080 system screenshot showed
   the desktop and a Disk Insertion dialog. The USB volume was
   mounted and its rollback and 720p backup checksums passed.
   The GPU startup marker failed again, with the same stalled ring
   read pointer and no startup fence. The repaint stayed unarmed.
   The first boot's complete GPU and framebuffer readbacks stand.
   The separate post-copy on-screen check remains unconfirmed.
   Startup analysis found the MEM_WRITE packet consumed at 0x00c7
   with its CPU scratch sentinel unchanged. The ring later stalled
   at 0x03df while the idle timer kept advancing the write pointer.
   This happened before any WindowServer repaint. The old driver
   ignored hardware and firmware initialization errors, and it
   stopped the platform framebuffer without verifying GPU writes.
   The observations do not isolate the initial register or timing
   fault. The gpuringfix profile stages XenonCoreGraphics 0.0.5d46
   with a read-backed halt and reset, explicit ring-pointer reset,
   checked firmware loading, and GPU memory-write markers before
   and after framebuffer setup. It retries startup up to three
   times and retains the software framebuffer if verification fails.
   The idle display timer now waits for the queued batch to drain.
   Tiger unpacked all nine kexts, verified eight unchanged executables
   and passed both graphics kext offline link checks. The new archive
   is 1,248 bytes larger, and the OpenBIOS image differs only in its
   archive filename. All 55 earlier boot entries and all 463 existing
   USB files matched across staging. gpuringfix is now selected
   for the cold boot. kboot.conf.before-gpuringfix.bak preserves
   gpuwinbatch, and the separate gpuwinfull rollback remains
   available. All 467 protected files matched across activation,
   including native HDMI, earlier tests and the 720p backup.
   gpuringfix cold-booted with XenonCoreGraphics 0.0.5d46 and
   XenonFramebufferTest 0.0.5d3 loaded. WindowServer, Finder and
   Dock were reachable over SSH, and the console desktop was slow.
   XenosRingStartupReady was No, and the kernel logged 0xe00002be
   (kIOReturnNoResources) before the first command-processor
   reset attempt. This status covers ring-buffer allocation and
   the virtual address, physical address, alignment, segment length
   and range checks. Existing logs do not identify the failed
   check. The driver retained the CPU-driven platform framebuffer.
   It executed neither startup marker or WindowServer startup
   clear. The 29-batch repaint stayed unarmed. gpuringfix remains
   active, and both rollback configs, native HDMI and the 720p
   backup passed their post-boot checksum checks.
   Darwin 8.11.0 allocator source confirms that the 32 KiB alignment
   mask applies to the virtual mapping. Its physical page allocation
   does not receive the mask. Physical alignment is therefore the
   leading explanation for d46's early rejection. The old log has no
   descriptor address or segment length to establish the exact check.
   gpuringbuf stages XenonCoreGraphics 0.0.5d47. It records the d46
   check result on the first allocation, verifies physical continuity
   per page, and can select a 32 KiB-aligned window from a 60 KiB
   contiguous allocation. The selected ring must fit before the
   video buffers, and the startup markers still gate GPU work.
   Local allocation tests passed at all eight page offsets and
   rejected physical gaps and out-of-range addresses. Tiger passed
   both graphics kext offline links, and the other eight kexts
   remained byte-identical. The alternate profile retains all 56
   existing boot entries. All 469 existing USB files matched across
   staging, including the active gpuringfix profile, gpuwinfull
   rollback, native HDMI, earlier tests and 720p backup.
   gpuringbuf is selected for the cold boot. All 473 protected files
   matched across activation. kboot.conf.before-gpuringbuf.bak
   preserves gpuringfix, and the gpuwinfull rollback remains
   available. The cold boot loaded XenonCoreGraphics 0.0.5d47, and
   the original ring check rejected physical alignment. Its 32 KiB
   segment began at 0x0173c000. The aligned-window fallback selected
   0x016b8000, 28,672 bytes into the larger allocation.
   Both GPU memory-write markers passed on attempt 1 and the ring
   drained after each. WindowServer PID 72 completed startup fences
   1 and 2, checking 1,024 pixels each with zero mismatches.
   The 29-batch overlapping repaint passed over SSH. WindowServer
   owned ordered fences 3 through 31, and all 7,252 clipped pixels
   matched both GPU and framebuffer readbacks (hash eeb35d15).
   The independent on-screen check matched all 6,912 content pixels.
   The GPU batches took 4.276 seconds. The repaint marker and lock
   were removed. Quartz Extreme remained off, so milestone 7 and
   the desktop smoke test remain open.
   gpuwintile stages XenonCoreGraphics 0.0.5d48, XenosWinTile.plugin,
   WINTILE.ELF and XWTC.MKE. It uses the same 7,252-pixel overlapping
   repaint and a new 32 × 32 capture selector. The test targets eight
   GPU submissions instead of 29, with 1,024 pixels per submission
   and 84 covered pixels in the last tile. The driver checks every
   tile and the plug-in checks all clipped GPU and framebuffer pixels.
   The verifier also checks 6,912 independent on-screen content pixels,
   eight ordered fence receipts, GPU submission-and-readback time,
   and the time from move request to the visible result. Its live
   timing comparison uses the 4,276 ms gpuringbuf repaint.
   The previous 29-submit offscreen fixture reran over SSH in
   4,284 ms with all 7,252 pixels and 9,132 guards matching. The new
   plug-in's Tiger factory, timing callbacks, non-owner aperture
   check, and unarmed overlapping move passed on the current boot.
   Tiger passed both graphics kext offline links and checked eight
   unchanged executables. All 475 existing USB files matched across
   staging, including gpuringbuf, earlier profiles, native HDMI and
   the 720p backup. The alternate config retains all 57 earlier
   boot entries. XenosWinTile.plugin is installed on Tiger with
   matching checksums and root:wheel ownership. gpuwintile is
   selected for the cold boot. kboot.conf.before-gpuwintile.bak
   preserves gpuringbuf, and all 484 protected files matched across
   activation. The gpuwintile cold boot loaded XenonCoreGraphics
   0.0.5d48 and XenosWinTile. The physically aligned ring fallback
   worked, both startup markers passed on the first attempt, and
   WindowServer PID 76 completed startup fences 1 and 2.
   The full-tile repaint passed over SSH with eight ordered fences,
   3 through 10. Each GPU tile checked 1,024 pixels with zero
   mismatches. All 7,252 clipped pixels matched in both readbacks
   (region hash eeb35d15), and the independent on-screen check
   matched all 6,912 window-content pixels. Submission and readback
   took 1,086 ms versus 4,276 ms for the 29-submission reference
   (3.94× faster). The move-to-visible check took 1,169 ms.
   gpuwintile remains active. The gpuringbuf rollback, native HDMI
   and 720p backup passed post-boot checks. Quartz Extreme stayed
   off, so milestone 7 and the desktop smoke test remain open.
8. Desktop smoke test — exercise window movement, overlap, resize and
   720p/1080p switching with acceleration active.

These are eight planned integration milestones. Each may take more than one
hardware run, especially Tiger renderer discovery and WindowServer integration.


## SMP follow-up

- [XenonPE::refreshFramebuffer](../XenonPlatform/src/PE/XenonPE_GPU.cpp#L50)
  disables interrupts only on the current CPU while copying scanout. Audit
  serialization between active refresh callbacks and framebuffer ownership or
  mode changes before enabling this path with multiple CPUs. Use a real lock
  or serialize both operations on the same work loop.
- [XenosController](../XenonCoreGraphics/src/XenosController.cpp) serializes
  submissions through its command gate and timer work loop. Audit display
  ownership transitions and callback teardown during SMP boot and mode
  changes, including transitions to the platform software framebuffer.
- [XenonFramebufferTest](../XenonFramebufferTest/src/XenonFramebufferTest.cpp)
  uses an owner lock for the private aperture. Exercise simultaneous opens,
  close and mode changes with SMP enabled before calling that path stable.
