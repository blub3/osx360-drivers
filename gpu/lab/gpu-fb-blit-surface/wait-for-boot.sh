#!/bin/sh
set -eu
cd "$(dirname "$0")"
old=$1
case "$old" in ''|*[!0-9]*) exit 2 ;; esac
tiger=../gpu-cgl-discovery/tiger-ssh
deadline=$(($(date +%s) + 660))
last=
while [ "$(date +%s)" -lt "$deadline" ]; do
    if status=$("$tiger" /bin/sh < boot-status-remote.sh 2>/dev/null) &&
       [ "${status%% *}" -gt "$old" ]; then
        if [ "$status" = "$last" ]; then
            printf 'Stable SSH and desktop process IDs, boot WindowServer Finder Dock: %s\n' "$status"
            exit 0
        fi
        last=$status
        sleep 18
    else
        last=
        sleep 8
    fi
done
echo "A fresh Tiger desktop with OSX360BOOT did not become reachable over SSH."
exit 4
