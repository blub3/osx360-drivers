#!/bin/sh
set -eu
cd "$(dirname "$0")"
case "${1-}" in ''|*[!0-9]*) exit 2 ;; esac
deadline=$(($(date +%s) + 720))
while test "$(date +%s)" -lt "$deadline"; do
    if output=$(../gpu-cgl-discovery/tiger-ssh "
        set -eu
        now=\$(/usr/sbin/sysctl -n kern.boottime)
        test \"\$now\" -gt $1
        test -x /Volumes/OSX360BOOT/XenosWindowServerTest/XenosWindowServerCheck
        printf 'Tiger boot time %s\\n' \"\$now\"
        /usr/sbin/kextstat | grep -F fish.goldfish64.osx360.XenonCoreGraphics
    " 2>/dev/null); then
        printf '%s\n' "$output"
        case "$output" in
            *'(0.0.5d39)'*)
                if ./verify-windowserver-on-tiger.sh; then
                    exit 0
                fi
                # Tiger may open SSH before WindowServer has initialized the display.
                sleep 12
                exec ./verify-windowserver-on-tiger.sh
                ;;
            *) echo 'Tiger started with a different XenonCoreGraphics version.' >&2; exit 3 ;;
        esac
    fi
    sleep 10
done
echo 'Timed out waiting for the new Tiger boot and the USB test.' >&2
exit 4
