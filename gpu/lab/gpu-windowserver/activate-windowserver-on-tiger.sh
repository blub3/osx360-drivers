#!/bin/sh
set -eu
cd "$(dirname "$0")"
../gpu-cgl-discovery/tiger-ssh '
set -eu
cd /Volumes/OSX360BOOT
config=kboot-gpu-windowserver.conf
backup=kboot.conf.before-gpuxenosws.bak
old=2a15e64f7e185e0790d5780b578fb016
new=cc2236851740a9c4c1a4b793b82ddd48
test "$(md5 -q kboot.conf)" = "$old"
test "$(md5 -q "$config")" = "$new"
test "$(md5 -q XenosQES.mkext)" = 30d4ab4c38ce4b852e195a11282c5308
test "$(md5 -q openbios-xbox360-gpu-windowserver.elf)" = 5bec537b95a8c8da04a557dbcd74121c
test "$(md5 -q openbios-xbox360-native-hdmi-test.elf)" = ca72c036347285e3fe77b58d3b2c93c6
test "$(md5 -q openbios-xbox360.720p-hdmi.elf.bak)" = 2ce8cf67dd57d0ef8b68beaa342e051a
test "$(md5 -q kboot.conf.before-gpumulticlient.bak)" = 8c2c7324d86de82840dede3d238135e8
test -x XenosWindowServerTest/XenosWindowServerCheck
test "$(md5 -q XenosCGL.mkext)" = 8ac34c28a1cc94f3ccd7bf9be6eeaa64
test "$(md5 -q kboot.conf.before-gpucgloffscreen.bak)" = 2da466ec6ff69691f4c8cfa3ce29cf3f
if ! test -e "$backup"; then
    cp -n kboot.conf "$backup"
fi
test "$(md5 -q "$backup")" = "$old"
temp=$(mktemp /Volumes/OSX360BOOT/.kboot-gpuxenosws.XXXXXXXX)
trap "rm -f \"$temp\"" 0
cat "$config" > "$temp"
test "$(md5 -q "$temp")" = "$new"
test "$(md5 -q kboot.conf)" = "$old"
sync
mv -f "$temp" kboot.conf
sync
test "$(md5 -q kboot.conf)" = "$new"
test "$(md5 -q "$backup")" = "$old"
grep -E "^default=|^gpuxenosws=|^gpucgloffscreen=" kboot.conf
/usr/sbin/sysctl -n kern.boottime
'
