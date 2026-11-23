set -eu
export LC_ALL=C
boot=/Volumes/OSX360BOOT
/sbin/mount | grep -Fq " on $boot ("
cd "$boot"
test "$(pwd -P)" = "$boot"
printf 'current_boot=%s\n' "$(/usr/sbin/sysctl -n kern.boottime)"
grep -Fx 'default=gpucompositor' kboot.conf
verify() { test "$(md5 -q "$2")" = "$1" || { echo "Checksum failed for $2" >&2; exit 1; }; }
verify d4e5729d0e58cd7b12194d3469f59509 kboot.conf
verify 1c9b0443b89ba18117700c59b2e6e577 kboot.conf.before-gpucompositor.bak
verify 1d13710c7771021b6d4bd50db57580e0 XenosWCC.mkext
verify 7a6ed9d448391eb911a124abb4d4edd8 openbios-xbox360-gpu-compositor-context.elf
verify 9d6c4d2bc106bc335b6e4f840b0a7083 XenosWFR.mkext
verify b99d4be9377e3e7965bb3e88d5f1ff0e NativeHD.mkext
verify ca72c036347285e3fe77b58d3b2c93c6 openbios-xbox360-native-hdmi-test.elf
verify 2ce8cf67dd57d0ef8b68beaa342e051a openbios-xbox360.720p-hdmi.elf.bak
extensions=/System/Library/Extensions
for entry in \
    '477f768ca2520245964f362c6477aa1e GLDriverXenosCompositor' \
    '653ae06ff70d3e16e62f9f0c6cbfc478 GLDriverXenosNative' \
    'd3ec0ee7b5cc2013e0c256c0d9c742f1 GLDriverXenosWSProbe'; do
    checksum=${entry%% *}
    executable=${entry#* }
    verify "$checksum" "$extensions/$executable.bundle/Contents/MacOS/$executable"
done
kexts=$(/usr/sbin/kextstat)
printf '%s\n' "$kexts" | grep -E 'fish.goldfish64.osx360.Xenon(FramebufferTest|CoreGraphics)' || :
/usr/sbin/ioreg -w0 -c XenonFramebufferTest | grep -E 'XenosWindowServerFramebufferPID|IOGLBundleName' || :
/sbin/dmesg | grep -Ei 'XenonCoreGraphics|XenonFramebufferTest|XenosWindowServer|XenosCompositorGLD|native HDMI|undefined' | tail -40 || :
if ! printf '%s\n' "$kexts" | grep -Fq 'fish.goldfish64.osx360.XenonCoreGraphics (0.0.5d45)' ||
   ! printf '%s\n' "$kexts" | grep -Fq 'fish.goldfish64.osx360.XenonFramebufferTest (0.0.5d2)'; then
    exit 3
fi
cd XenosCompositorContextTest
verify 63f961d214d4ee5be9f77a6e30d83b0a XenosCompositorRepaintCheck
verify 3cae3354bb4902e933cf672f6d7ce728 RunXenosCompositorRepaint.sh
status=0
if [ -r /var/log/system.log ]; then
    log_size=$(wc -c < /var/log/system.log | tr -d ' ')
fi
/bin/sh RunXenosCompositorRepaint.sh --repaint || status=$?
test ! -e repaint-probe.arm
/usr/sbin/ioreg -w0 -c XenosController | grep -E 'XenosWindowServerFramebuffer(Trace|QueuedPID|Probe)|XenosGPUCompletion' || :
if [ -r /var/log/system.log ]; then
    echo 'Recent compositor log entries'
    tail -350 /var/log/system.log | grep -F XenosCompositorGLD | tail -26 || :
    echo 'Compositor log entries during the repaint run'
    tail -c "+$((log_size + 1))" /var/log/system.log | grep -F XenosCompositorGLD || :
fi
printf 'WindowServer repaint status=%s\n' "$status"
exit "$status"
