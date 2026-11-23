#!/bin/sh
set -eu
cd "$(dirname "$0")"
../gpu-cgl-discovery/tiger-ssh /bin/sh <<'REMOTE'
set -eu
export LC_ALL=C
cd /Volumes/OSX360BOOT
printf 'current_boot=%s\n' "$(/usr/sbin/sysctl -n kern.boottime)"
grep -Fx 'default=gpuwssubmit' kboot.conf
test "$(md5 -q kboot.conf)" = 6d5c258b4b5adb7af7f78c35c932bff9
test "$(md5 -q kboot.conf.before-gpuwssubmit.bak)" = 5790ac8b037422bf101f57fd9e51c55b
test "$(md5 -q kboot.conf.before-gpudisplaysurface.bak)" = 228691583a418df3e68ce58b5c8adca9
test "$(md5 -q XenosWSP.mkext)" = 18663e43a5af0009a381b562cb5ca66d
test "$(md5 -q XenosWSS.mkext)" = c717c0c0606b19b4eb13b48a403e107f
test "$(md5 -q NativeHD.mkext)" = b99d4be9377e3e7965bb3e88d5f1ff0e
test "$(md5 -q openbios-xbox360-native-hdmi-test.elf)" = ca72c036347285e3fe77b58d3b2c93c6
test "$(md5 -q openbios-xbox360.720p-hdmi.elf.bak)" = 2ce8cf67dd57d0ef8b68beaa342e051a
test "$(md5 -q XenosWindowServerSubmitTest/XenosWindowServerSubmitCheck)" = 1a0ae9ea902f9b778fb7046353b643da
test "$(md5 -q XenosWindowServerSubmitTest/RunXenosWindowServerSubmit.sh)" = 5003bb79f752b2b2f0f02132020bd8e1
bundle=/System/Library/Extensions/GLDriverXenosWSProbe.bundle/Contents
test "$(md5 -q "$bundle/Info.plist")" = 0360a00c8eb911aeb51258f3077c5185
test "$(md5 -q "$bundle/MacOS/GLDriverXenosWSProbe")" = d3ec0ee7b5cc2013e0c256c0d9c742f1
test "$(md5 -q /System/Library/Extensions/GLDriverXenosNative.bundle/Contents/MacOS/GLDriverXenosNative)" = 653ae06ff70d3e16e62f9f0c6cbfc478
echo 'Both GLD bundles, active profile, rollback, probe, native HDMI, and 720p checksums passed.'
if ! /usr/sbin/kextstat | grep -F 'fish.goldfish64.osx360.XenonCoreGraphics (0.0.5d43)'; then
    /usr/sbin/kextstat | grep -i Xenon || :
    /sbin/dmesg | grep -Ei 'XenonCoreGraphics|XenosWindowServer|GLDriver' || :
    exit 3
fi

cd XenosWindowServerSubmitTest
unset GL_RESOURCES XENOS_NATIVE_GLD_PROBE DYLD_INSERT_LIBRARIES DYLD_FRAMEWORK_PATH DYLD_LIBRARY_PATH
startup=0
compositor=0
/bin/sh RunXenosWindowServerSubmit.sh --startup || startup=$?
/bin/sh RunXenosWindowServerSubmit.sh --require-native || compositor=$?
printf 'Startup ownership status %s, Quartz Extreme acceptance status %s\n' "$startup" "$compositor"
/usr/sbin/ioreg -w0 -c XenosDisplayAccelerator |
    grep -E 'XenosDisplayAccelerator|IOGLBundleName|AccelCaps|XenosWindowServerStartup' || :
/sbin/dmesg | grep -Ei 'XenosWindowServer|XenosSurface:|native HDMI' | tail -25 || :
if test -r /var/log/system.log; then
    grep -E 'XenosWindowServerStartup|cglsInitialize|cglsChoosePixelFormat' /var/log/system.log | tail -18 || :
fi
if test -r /var/log/windowserver.log; then
    tail -90 /var/log/windowserver.log |
        grep -Ei 'CGLS|AccelCaps|quartz extreme|GLDriverXenosWSProbe|GLEngine|XenosWindowServerStartup' | tail -20 || :
fi
if test "$startup" -ne 0; then
    exit 5
fi
if test "$compositor" -ne 0; then
    exit 6
fi
REMOTE
