#!/bin/sh
set -eu
cd "$(dirname "$0")"
old=$1
case "$old" in ''|*[!0-9]*) exit 2 ;; esac
deadline=$(($(date +%s) + 780))
while [ "$(date +%s)" -lt "$deadline" ]; do
    if boot=$(../gpu-cgl-discovery/tiger-ssh '
        boot=$(/usr/sbin/sysctl -n kern.boottime) || exit 1
        test -f /Volumes/OSX360BOOT/XenosDisplaySurfaceTest/XenosDisplaySurfaceCheck || exit 1
        /bin/ps -axww -o pid,command |
            /usr/bin/awk '\''$2 ~ /\/WindowServer$/ && $3 == "-daemon" { found=1 } END { exit !found }'\'' || exit 1
        printf "%s\n" "$boot"
    ' 2>/dev/null) && [ "$boot" -gt "$old" ]; then
        exec /bin/sh verify-surface-on-tiger.sh
    fi
    sleep 10
done
echo "Timed out waiting for a new Tiger boot with WindowServer and OSX360BOOT available."
exit 4
