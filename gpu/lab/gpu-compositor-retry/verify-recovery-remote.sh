set -eu
export LC_ALL=C
/sbin/mount | grep -Fq ' on /Volumes/OSX360BOOT ('
cd /Volumes/OSX360BOOT
printf 'current_boot=%s\n' "$(/usr/sbin/sysctl -n kern.boottime)"
grep -Fx 'default=gpucompositor' kboot.conf
test "$(md5 -q kboot.conf)" = d4e5729d0e58cd7b12194d3469f59509
test "$(md5 -q kboot.conf.before-gpucompretry.bak)" = d4e5729d0e58cd7b12194d3469f59509
test "$(md5 -q kboot.conf.before-gpucompositor.bak)" = 1c9b0443b89ba18117700c59b2e6e577
test "$(md5 -q NativeHD.mkext)" = b99d4be9377e3e7965bb3e88d5f1ff0e
test "$(md5 -q openbios-xbox360-native-hdmi-test.elf)" = ca72c036347285e3fe77b58d3b2c93c6
test "$(md5 -q openbios-xbox360.720p-hdmi.elf.bak)" = 2ce8cf67dd57d0ef8b68beaa342e051a
/usr/sbin/kextstat | grep -E 'fish.goldfish64.osx360.Xenon(FramebufferTest|CoreGraphics)'
/usr/sbin/ioreg -w0 -c XenonFramebufferTest | grep -E 'XenosWindowServerFramebufferPID|IOGLBundleName'
ps -axww -o pid,command | awk '$2 ~ /\/(WindowServer|Finder|Dock)$/ { print $1, $2 }'
test ! -e XenosCompositorRetryTest/transition-probe.arm
test ! -e XenosCompositorRetryTest/repaint-probe.arm
echo 'Recovery boot and protected HDMI checksums verified.'
