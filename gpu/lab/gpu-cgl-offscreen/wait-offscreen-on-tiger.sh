#!/bin/sh
set -eu
cd "$(dirname "$0")"
case "${1-}" in ''|*[!0-9]*) exit 2 ;; esac
deadline=$(($(date +%s) + 720))
while test "$(date +%s)" -lt "$deadline"; do
    if result=$(../gpu-cgl-discovery/tiger-ssh "
        set -eu
        now=\$(/usr/sbin/sysctl -n kern.boottime)
        test \"\$now\" -gt $1
        test -x /Volumes/OSX360BOOT/XenosCGLOffscreenTest/XenosCGLOffscreen
        /usr/sbin/kextstat | grep -F fish.goldfish64.osx360.XenonCoreGraphics
    " 2>&1); then
        printf '%s\n' "$result"
        case "$result" in
            *'(0.0.5d38)'*) exec ./verify-offscreen-on-tiger.sh ;;
        esac
        echo 'Tiger started with a different XenonCoreGraphics version.' >&2
        exit 3
    fi
    sleep 10
done
echo 'Timed out waiting for the new Tiger boot and USB test.' >&2
exit 4
