#!/bin/sh
set -eu
umask 077
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
scratch=$(mktemp -d "$here/.linked-quad-batch-trilinear.XXXXXX")
trap 'rm -rf "$scratch"' EXIT
ssh -T -o ConnectTimeout=8 -o ServerAliveInterval=8 -o ServerAliveCountMax=2 \
    -o StrictHostKeyChecking=yes -o UserKnownHostsFile="$here/../tx-build/known_hosts_g5" \
    -o HostKeyAlgorithms=+ssh-rsa ${OSX360_G5_HOST} \
    "$(cat "$here/link-remote.sh")" < "$here/link-input.tar" > "$scratch/build.tar"
tar -xf "$scratch/build.tar" -C "$scratch"
file "$scratch/XenonCoreGraphics-linked" | grep 'Mach-O object ppc'
file "$scratch/XenosQuadBatchTrilinear" | grep 'Mach-O executable ppc'
mv "$scratch/XenonCoreGraphics-linked" "$scratch/XenosQuadBatchTrilinear" "$here/"
echo 'GPU quad trilinear kext and Tiger app build complete.'
