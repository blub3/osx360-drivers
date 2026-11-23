#!/bin/sh
set -eu
cd "$(dirname "$0")"
../gpu-cgl-discovery/tiger-ssh /bin/sh <<'REMOTE'
set -eu
export LC_ALL=C
boot=/Volumes/OSX360BOOT
bundle=/System/Library/Extensions/GLDriverXenosNative.bundle/Contents
printf 'current_boot=%s\n' "$(/usr/sbin/sysctl -n kern.boottime)"
cd "$boot"
grep -Fx 'default=gpunativegld' kboot.conf
test "$(md5 -q kboot.conf)" = 228691583a418df3e68ce58b5c8adca9
test "$(md5 -q kboot.conf.before-gpunativegld.bak)" = 6f70043baec8ea1b5280136cf16856be
test "$(md5 -q XenosCGL.mkext)" = 8ac34c28a1cc94f3ccd7bf9be6eeaa64
test "$(md5 -q NativeHD.mkext)" = b99d4be9377e3e7965bb3e88d5f1ff0e
test "$(md5 -q openbios-xbox360.720p-hdmi.elf.bak)" = 2ce8cf67dd57d0ef8b68beaa342e051a
test "$(md5 -q "$bundle/Info.plist")" = b67cb572421876ce39a9213d7800ae8f
test "$(md5 -q "$bundle/MacOS/GLDriverXenosNative")" = 653ae06ff70d3e16e62f9f0c6cbfc478
echo 'Installed bundle, active profile, rollback, native HDMI, and 720p checksums passed.'
if ! /usr/sbin/kextstat | grep -F 'fish.goldfish64.osx360.XenonCoreGraphics (0.0.5d41)'; then
    /usr/sbin/kextstat | grep -i Xenon || :
    /sbin/dmesg | grep -Ei 'XenonCoreGraphics|XenosWindowServer|GLDriver' || :
    exit 3
fi
registry=$(/usr/sbin/ioreg -w0 -c IOAccelerator)
printf '%s\n' "$registry" | grep -E 'XenosDisplayAccelerator|IOGLBundleName|IOAccelTypes' || :
cd XenosNativeGLDTest
unset GL_RESOURCES XENOS_NATIVE_GLD_PROBE DYLD_INSERT_LIBRARIES DYLD_FRAMEWORK_PATH DYLD_LIBRARY_PATH
echo "Checking stock CGL lookup with default resource paths."
first=0
/bin/sh RunXenosNativeGLD.sh --native || first=$?
if [ "$first" -ne 0 ]; then
    /usr/sbin/ioreg -w0 -c XenonFramebufferTest | grep -E 'XenonFramebufferTest|IOGLBundleName|IOAccelTypes' || :
    /sbin/dmesg | grep -Ei 'XenosWindowServer|GLDriver|native HDMI' || :
    exit "$first"
fi
/bin/sh RunXenosNativeGLD.sh --native
/sbin/dmesg | grep -E 'XenosWindowServer|native HDMI' | tail -8
REMOTE
