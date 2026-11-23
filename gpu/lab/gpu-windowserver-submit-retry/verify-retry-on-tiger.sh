#!/bin/sh
set -eu
cd "$(dirname "$0")"
../gpu-cgl-discovery/tiger-ssh /bin/sh <<'REMOTE'
set -eu
export LC_ALL=C
cd /Volumes/OSX360BOOT
printf 'current_boot=%s\n' "$(/usr/sbin/sysctl -n kern.boottime)"
grep -Fx 'default=gpuwsfbretry' kboot.conf
test "$(md5 -q kboot.conf)" = 1c9b0443b89ba18117700c59b2e6e577
test "$(md5 -q kboot.conf.before-gpuwsfbretry.bak)" = 6d5c258b4b5adb7af7f78c35c932bff9
test "$(md5 -q kboot.conf.before-gpuwssubmit.bak)" = 5790ac8b037422bf101f57fd9e51c55b
test "$(md5 -q XenosWFR.mkext)" = 9d6c4d2bc106bc335b6e4f840b0a7083
test "$(md5 -q XenosWSP.mkext)" = 18663e43a5af0009a381b562cb5ca66d
test "$(md5 -q NativeHD.mkext)" = b99d4be9377e3e7965bb3e88d5f1ff0e
test "$(md5 -q openbios-xbox360-native-hdmi-test.elf)" = ca72c036347285e3fe77b58d3b2c93c6
test "$(md5 -q openbios-xbox360.720p-hdmi.elf.bak)" = 2ce8cf67dd57d0ef8b68beaa342e051a
test "$(md5 -q XenosWindowServerFramebufferTest/XenosWindowServerFramebufferCheck)" = b720575eeafb67f95308da859fabf4bc
test "$(md5 -q XenosWindowServerFramebufferTest/RunXenosWindowServerFramebuffer.sh)" = e0847dc088d105d680a3f484eea51bbe
test "$(md5 -q /System/Library/Extensions/GLDriverXenosNative.bundle/Contents/MacOS/GLDriverXenosNative)" = 653ae06ff70d3e16e62f9f0c6cbfc478
test "$(md5 -q /System/Library/Extensions/GLDriverXenosWSProbe.bundle/Contents/MacOS/GLDriverXenosWSProbe)" = d3ec0ee7b5cc2013e0c256c0d9c742f1
echo 'Both GLD bundles, active profile, rollback, probe, native HDMI, and 720p checksums passed.'
kexts=$(/usr/sbin/kextstat)
printf '%s\n' "$kexts" | grep -E 'fish.goldfish64.osx360.Xenon(FramebufferTest|CoreGraphics)' || :
if ! printf '%s\n' "$kexts" | grep -Fq 'fish.goldfish64.osx360.XenonCoreGraphics (0.0.5d44)' ||
   ! printf '%s\n' "$kexts" | grep -Fq 'fish.goldfish64.osx360.XenonFramebufferTest (0.0.5d2)'; then
    /sbin/dmesg | grep -Ei 'XenonCoreGraphics|XenonFramebufferTest|XenosWindowServer|undefined' || :
    exit 3
fi
cd XenosWindowServerFramebufferTest
unset GL_RESOURCES XENOS_NATIVE_GLD_PROBE DYLD_INSERT_LIBRARIES DYLD_FRAMEWORK_PATH DYLD_LIBRARY_PATH
startup=0
/bin/sh RunXenosWindowServerFramebuffer.sh --startup || startup=$?
printf 'Framebuffer open and startup fence ownership status %s\n' "$startup"
/usr/sbin/ioreg -w0 -c XenonFramebufferTest | grep -E 'XenosWindowServerFramebufferPID|IOGLBundleName' || :
/usr/sbin/ioreg -w0 -c XenosController |
    grep -E 'XenosWindowServerFramebuffer(Trace|QueuedPID|Probe)|XenosGPUCompletion' || :
/sbin/dmesg | grep -Ei 'XenosWindowServer|WindowServer framebuffer open|XenosSurface:|native HDMI' | tail -25 || :
exit "$startup"
REMOTE
