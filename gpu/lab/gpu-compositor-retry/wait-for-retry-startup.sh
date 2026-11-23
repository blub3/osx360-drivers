#!/bin/sh
set -eu
cd "$(dirname "$0")"
old=$1
case "$old" in ''|*[!0-9]*) exit 2 ;; esac
deadline=$(($(date +%s) + 780))
while [ "$(date +%s)" -lt "$deadline" ]; do
    if boot=$(../gpu-cgl-discovery/tiger-ssh '
        test -f /Volumes/OSX360BOOT/XenosCompositorRetryTest/XenosTransitionProbe || exit 1
        /bin/ps -axww -o pid,command |
            /usr/bin/awk '\''$2 ~ /\/WindowServer$/ && $3 == "-daemon" { found=1 } END { exit !found }'\'' || exit 1
        /usr/sbin/sysctl -n kern.boottime
    ' 2>/dev/null) && [ "$boot" -gt "$old" ]; then
        printf 'New Tiger boot with mounted USB volume and WindowServer: %s\n' "$boot"
        exit 0
    fi
    sleep 10
done
echo "A fresh Tiger boot with WindowServer and OSX360BOOT did not become reachable over SSH."
exit 4
