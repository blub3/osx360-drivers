#!/bin/sh
set -eu
cd "$(dirname "$0")"
../gpu-cgl-discovery/tiger-ssh '
set -eu
driver=$(/usr/sbin/kextstat | grep -F "fish.goldfish64.osx360.XenonCoreGraphics (0.0.5d38)")
printf "%s\n" "$driver"
cd /Volumes/OSX360BOOT
test "$(md5 -q XenosCGL.mkext)" = 8ac34c28a1cc94f3ccd7bf9be6eeaa64
test "$(md5 -q XenosCGLOffscreenTest/XenosCGLOffscreen)" = 100b7ff48d721d91283fb51206d72e67
test "$(md5 -q XenosCGLOffscreenTest/GLEngineXenos)" = 444937ea4bf5896c20f676cda9cac818
/bin/sh XenosCGLOffscreenTest/RunXenosCGLOffscreen.sh
dmesg | grep "XenosGeometryQuad:" | tail -2
'
