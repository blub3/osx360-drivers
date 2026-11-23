#!/bin/sh
set -eu
umask 077
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
stage=$(mktemp "$here/.linked-xenos-alpha-blend.XXXXXX")
trap 'rm -f "$stage"' EXIT
ssh -T -o ConnectTimeout=8 -o ServerAliveInterval=8 -o ServerAliveCountMax=2 \
    -o StrictHostKeyChecking=yes -o UserKnownHostsFile="$here/../tx-build/known_hosts_g5" \
    -o HostKeyAlgorithms=+ssh-rsa ${OSX360_G5_HOST} \
    "$(cat "$here/link-remote.sh")" < "$here/link-input.tar" > "$stage"
file "$stage" | grep 'Mach-O object ppc'
mv "$stage" "$here/XenonCoreGraphics-linked"
echo 'GPU alpha blend kext link complete.'
