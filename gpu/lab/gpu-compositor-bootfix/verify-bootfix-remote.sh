set -eu
export LC_ALL=C
boot=/Volumes/OSX360BOOT
/sbin/mount | grep -Fq " on $boot ("
cd "$boot"
test "$(pwd -P)" = "$boot"
printf 'current_boot=%s\n' "$(/usr/sbin/sysctl -n kern.boottime)"
grep -Fx 'default=gpucompbootfix' kboot.conf
verify() { test "$(md5 -q "$2")" = "$1" || { echo "Checksum failed for $2" >&2; exit 1; }; }
verify c76086865a738b203ebe3aab6f628dd5 kboot.conf
verify d4e5729d0e58cd7b12194d3469f59509 kboot.conf.before-gpucompbootfix.bak
verify 3d5be96c3df8c4c3cdfc514c7c637ba1 CMPFIX.ELF
verify c603562c9452658c09f5e25db66ff936 XWCF.MKE
verify d4e5729d0e58cd7b12194d3469f59509 kboot.conf.before-gpucompretry.bak
verify 1c9b0443b89ba18117700c59b2e6e577 kboot.conf.before-gpucompositor.bak
verify cf2b0b281476ebd922e9d07dc979c7e0 XenosWCR.mkext
verify 10cb161defa1d9b11f544ef5bf548f93 openbios-xbox360-gpu-compositor-retry.elf
verify 1d13710c7771021b6d4bd50db57580e0 XenosWCC.mkext
verify 7a6ed9d448391eb911a124abb4d4edd8 openbios-xbox360-gpu-compositor-context.elf
verify b99d4be9377e3e7965bb3e88d5f1ff0e NativeHD.mkext
verify ca72c036347285e3fe77b58d3b2c93c6 openbios-xbox360-native-hdmi-test.elf
verify 2ce8cf67dd57d0ef8b68beaa342e051a openbios-xbox360.720p-hdmi.elf.bak
extensions=/System/Library/Extensions
verify 67af5267dc7340d3c5dfc99e488cd404 "$extensions/GLDriverXenosCompositorRetry.bundle/Contents/MacOS/GLDriverXenosCompositorRetry"
verify 18b8452094cb5fb131fd5ceb47c5acda "$extensions/GLDriverXenosCompositorRetry.bundle/Contents/Info.plist"
verify 477f768ca2520245964f362c6477aa1e "$extensions/GLDriverXenosCompositor.bundle/Contents/MacOS/GLDriverXenosCompositor"
kexts=$(/usr/sbin/kextstat)
printf '%s\n' "$kexts" | grep -E 'fish.goldfish64.osx360.Xenon(FramebufferTest|CoreGraphics)'
printf '%s\n' "$kexts" | grep -Fq 'fish.goldfish64.osx360.XenonCoreGraphics (0.0.5d45)'
printf '%s\n' "$kexts" | grep -Fq 'fish.goldfish64.osx360.XenonFramebufferTest (0.0.5d2)'
fb=$(/usr/sbin/ioreg -w0 -c XenonFramebufferTest)
printf '%s\n' "$fb" | grep -E 'XenosWindowServerFramebufferPID|IOGLBundleName'
printf '%s\n' "$fb" | grep -Fq '"IOGLBundleName" = "GLDriverXenosCompositorRetry"'
cd XenosCompositorRetryTest
verify 2f680340b10bcf37a115696927f5730e XenosTransitionProbe
verify d226726e7cf13975a14229a8e4d12f54 RunXenosTransitionProbe.sh
verify 0aa3610bb1b1315b8fec96f0a471e9d5 XenosCompositorRepaintCheck
verify 24e12d22b2442461dc22665016670415 RunXenosCompositorRepaint.sh
transition=0
/bin/sh RunXenosTransitionProbe.sh || transition=$?
test ! -e transition-probe.arm
echo "WindowServer_transition_status=$transition"
/usr/sbin/ioreg -w0 -c XenosController | grep -E 'XenosWindowServerFramebuffer(Trace|QueuedPID)|XenosGPUCompletion' || :
tail -200 /var/log/system.log | grep -E 'XenosCompositorRetryGLD (initialized|compositor format|context created|source=|FAIL)' || :
baseline=0
/bin/sh RunXenosCompositorRepaint.sh --baseline || baseline=$?
test ! -e repaint-probe.arm
printf 'Repaint_baseline_status=%s\n' "$baseline"
exit "$transition"
