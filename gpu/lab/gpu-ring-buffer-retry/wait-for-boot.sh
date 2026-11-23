#!/bin/sh
set -eu
cd "$(dirname "$0")"
old=$1
case "$old" in ''|*[!0-9]*) exit 2 ;; esac
tiger=../gpu-cgl-discovery/tiger-ssh
deadline=$(($(date +%s) + 660))
last=
while [ "$(date +%s)" -lt "$deadline" ]; do
    if status=$("$tiger" /bin/sh < boot-observe-remote.sh 2>/dev/null) &&
       [ "${status%% *}" -gt "$old" ]; then
        read -r boot registration mount server finder dock crashes <<EOF
$status
EOF
        if [ "$registration" = ringbuf ]; then
            if [ "$crashes" -ge 3 ] && { [ "$finder" -eq 0 ] || [ "$dock" -eq 0 ]; }; then
                printf 'WindowServer restart loop: %s\n' "$status"
                exit 5
            fi
            if [ "$server" -ne 0 ] && [ "$finder" -ne 0 ] && [ "$dock" -ne 0 ]; then
                if [ "$mount" = no ]; then
                    "$tiger" '
device=$(/usr/sbin/diskutil list | awk '\''$2 == "DOS_FAT_32" && $3 == "OSX360BOOT" {print $NF}'\'')
case "$device" in disk*s1) /usr/sbin/diskutil mount "$device";; *) exit 1;; esac
' >/dev/null 2>&1 || :
                elif [ "$status" = "$last" ]; then
                    printf 'Stable gpuringbuf boot, registration, volume, WindowServer, Finder, Dock, crashes: %s\n' "$status"
                    exit 0
                fi
                last=$status
                sleep 16
                continue
            fi
        fi
    fi
    last=
    sleep 8
done
echo "A fresh gpuringbuf desktop with OSX360BOOT did not become reachable over SSH."
exit 4
