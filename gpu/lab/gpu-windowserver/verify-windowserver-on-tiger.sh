#!/bin/sh
set -eu
cd "$(dirname "$0")"
../gpu-cgl-discovery/tiger-ssh '
set -eu
/usr/sbin/kextstat | grep -F "fish.goldfish64.osx360.XenonCoreGraphics (0.0.5d39)"
cd /Volumes/OSX360BOOT/XenosWindowServerTest
/bin/sh RunXenosWindowServer.sh --association
control=$(/bin/sh RunXenosWindowServer.sh --control)
printf "%s\n" "$control"
printf "%s\n" "$control" | grep -F "owner_trace=verified" >/dev/null
set +e
/bin/sh RunXenosWindowServer.sh --require-native
native_status=$?
set -e
printf "Native acceleration acceptance exit status %s\n" "$native_status"
test "$native_status" -eq 1
'
