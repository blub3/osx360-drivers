#!/bin/sh
set -eu
umask 077
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
stage=$(mktemp "$here/.linked-v4.XXXXXX")
trap 'rm -f "$stage"' EXIT
ssh -T -o ConnectTimeout=8 -o ServerAliveInterval=8 -o ServerAliveCountMax=2 \
    -o StrictHostKeyChecking=yes -o UserKnownHostsFile="$here/../tx-build/known_hosts_g5" \
    -o HostKeyAlgorithms=+ssh-rsa ${OSX360_G5_HOST} \
    "$(cat "$here/link-remote.sh")" < "$here/link-input-v4.tar" > "$stage"
file "$stage" | grep 'Mach-O object ppc'
mv "$stage" "$here/XenonFramebufferTest-linked-v4"
echo "Classic PowerPC link complete."
