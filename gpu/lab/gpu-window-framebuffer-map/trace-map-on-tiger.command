#!/bin/bash
set -euo pipefail
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
tiger="$here/../gpu-cgl-discovery/tiger-ssh"
source=$("$tiger" 'set -eu; f=$(mktemp /tmp/xenos-vram-source.XXXXXXXX); cat > "$f"; printf "%s\n" "$f"' < "$here/../gpu-window-repaint/test-map")
case "$source" in /tmp/xenos-vram-source.*) ;; *) exit 2 ;; esac
case "$source" in *[!a-zA-Z0-9/.-]*) exit 2 ;; esac
trap '"$tiger" "rm -f $source" >/dev/null' EXIT
script=$(cat "$here/trace-remote.sh")
case "$script" in *"'"*) echo "Cannot quote the Tiger trace." >&2; exit 1 ;; esac
ssh -tt -o BatchMode=yes -o IdentitiesOnly=yes -i "$HOME/.ssh/id_rsa_osx360" \
    -o ConnectTimeout=5 -o ServerAliveInterval=5 -o ServerAliveCountMax=2 \
    -o HostKeyAlgorithms=ssh-rsa -o PubkeyAcceptedAlgorithms=ssh-rsa \
    -o StrictHostKeyChecking=yes -o UserKnownHostsFile="$HOME/.ssh/known_hosts_osx360" \
    ${OSX360_TIGER_HOST} "/usr/bin/sudo /bin/sh -c '$script' xenos-trace $source" |
    tee "$here/map-trace.log"
