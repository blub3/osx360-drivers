#!/bin/sh
set -eu
set -o pipefail
cd "$(dirname "$0")"
manifest=$(python3 package-retry.py)
remote=$(cat stage-remote.sh)
COPYFILE_DISABLE=1 tar --no-xattrs --no-mac-metadata -C boot-stage -cf - \
    XenosQER.mkext openbios-xbox360-gpu-windowserver-retry.elf kboot-gpu-windowserver-retry.conf \
    XenosWindowServerRetryTest | ../gpu-cgl-discovery/tiger-ssh "manifest='$manifest'
$remote"
