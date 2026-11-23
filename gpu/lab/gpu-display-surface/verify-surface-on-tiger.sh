#!/bin/sh
set -eu
cd "$(dirname "$0")"
../gpu-cgl-discovery/tiger-ssh /bin/sh <<'REMOTE'
set -eu
export LC_ALL=C
cd /Volumes/OSX360BOOT
printf 'current_boot=%s\n' "$(/usr/sbin/sysctl -n kern.boottime)"
grep -Fx 'default=gpudisplaysurface' kboot.conf
test "$(md5 -q kboot.conf)" = 5790ac8b037422bf101f57fd9e51c55b
test "$(md5 -q kboot.conf.before-gpudisplaysurface.bak)" = 228691583a418df3e68ce58b5c8adca9
test "$(md5 -q kboot.conf.before-gpunativegld.bak)" = 6f70043baec8ea1b5280136cf16856be
test "$(md5 -q XenosWSS.mkext)" = c717c0c0606b19b4eb13b48a403e107f
test "$(md5 -q XenosGLD.mkext)" = cfc64a836626cfdef2793a0ad17f8da3
test "$(md5 -q NativeHD.mkext)" = b99d4be9377e3e7965bb3e88d5f1ff0e
test "$(md5 -q openbios-xbox360-native-hdmi-test.elf)" = ca72c036347285e3fe77b58d3b2c93c6
test "$(md5 -q openbios-xbox360.720p-hdmi.elf.bak)" = 2ce8cf67dd57d0ef8b68beaa342e051a
test "$(md5 -q XenosDisplaySurfaceTest/XenosDisplaySurfaceCheck)" = 83f7fc8bb02c198b3a87a92f2c180683
test "$(md5 -q XenosDisplaySurfaceTest/RunXenosDisplaySurface.sh)" = 992258307da8a3ff8f7e380a35b8d8f5
echo 'Active profile, rollback, probe, native HDMI, and 720p checksums passed.'
status=0
/bin/sh XenosDisplaySurfaceTest/RunXenosDisplaySurface.sh --probe || status=$?
/usr/sbin/kextstat | grep -F 'fish.goldfish64.osx360.XenonCoreGraphics' || :
/sbin/dmesg | grep -Ei 'XenosDisplaySurface|XenosSurface:|XenonCoreGraphics|XenosWindowServer|native HDMI' | tail -35 || :
exit "$status"
REMOTE
