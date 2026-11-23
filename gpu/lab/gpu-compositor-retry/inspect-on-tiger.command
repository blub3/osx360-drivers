#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
tiger=../gpu-cgl-discovery/tiger-ssh
pid=$("$tiger" 'ps -axww -o pid,command | /usr/bin/awk '\''$2 ~ /\/WindowServer$/ && $3 == "-daemon" { print $1 }'\''')
case "$pid" in ''|*[!0-9]*) echo "Could not identify the WindowServer process." >&2; exit 1 ;; esac
scratch=$("$tiger" '/usr/bin/mktemp -d /tmp/xenos-ws-state.XXXXXXXX')
case "$scratch" in
    /tmp/xenos-ws-state.[a-zA-Z0-9][a-zA-Z0-9][a-zA-Z0-9][a-zA-Z0-9][a-zA-Z0-9][a-zA-Z0-9][a-zA-Z0-9][a-zA-Z0-9]) ;;
    *) echo "Unexpected Tiger temporary path." >&2; exit 1 ;;
esac
trap '"$tiger" "rm -f $scratch/inspect && rmdir $scratch" >/dev/null 2>&1 || :' EXIT
"$tiger" "umask 077 && cat > $scratch/inspect && chmod 700 $scratch/inspect" < build/XenosWindowServerState
digest=$(md5 -q build/XenosWindowServerState)
ssh -tt -o BatchMode=yes -o IdentitiesOnly=yes -i "$HOME/.ssh/id_rsa_osx360" \
    -o ConnectTimeout=5 -o ServerAliveInterval=5 -o ServerAliveCountMax=2 \
    -o HostKeyAlgorithms=ssh-rsa -o PubkeyAcceptedAlgorithms=ssh-rsa \
    -o StrictHostKeyChecking=yes -o UserKnownHostsFile="$HOME/.ssh/known_hosts_osx360" \
    ${OSX360_TIGER_HOST} "/usr/bin/sudo /bin/sh -c '
set -eu
test \"\$(/sbin/md5 -q /System/Library/Frameworks/ApplicationServices.framework/Versions/A/Frameworks/CoreGraphics.framework/Versions/A/CoreGraphics)\" = 442f843a7f7212f948928d51285f869c
test \"\$(/sbin/md5 -q $scratch/inspect)\" = $digest
$scratch/inspect $pid
/usr/bin/vmmap $pid | /usr/bin/grep -E \"(CoreGraphics.framework/Versions/A/CoreGraphics|OpenGL.framework|GLDriverXenosCompositor|/Resources/WindowServer)\" || :
echo \"WindowServer compositor state check complete.\"
'" | tee windowserver-state.log
