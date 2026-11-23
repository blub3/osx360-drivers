#!/bin/sh
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
script=$(cat "$here/install-gld-remote.sh")
case "$script" in *"'"*) echo "Cannot quote the Tiger installer." >&2; exit 1 ;; esac
exec ssh -tt -o BatchMode=yes -o IdentitiesOnly=yes -i "$HOME/.ssh/id_rsa_osx360" \
    -o ConnectTimeout=5 -o ServerAliveInterval=5 -o ServerAliveCountMax=2 \
    -o HostKeyAlgorithms=ssh-rsa -o PubkeyAcceptedAlgorithms=ssh-rsa \
    -o StrictHostKeyChecking=yes -o UserKnownHostsFile="$HOME/.ssh/known_hosts_osx360" \
    ${OSX360_TIGER_HOST} "/usr/bin/sudo /bin/sh -c '$script'"
