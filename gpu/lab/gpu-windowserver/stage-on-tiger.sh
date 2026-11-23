#!/bin/sh
set -eu
set -o pipefail
cd "$(dirname "$0")"
manifest=$(python3 package-windowserver.py)
remote=$(cat stage-remote.sh)
COPYFILE_DISABLE=1 tar --no-xattrs --no-mac-metadata -C boot-stage -cf - \
    XenosQES.mkext openbios-xbox360-gpu-windowserver.elf kboot-gpu-windowserver.conf \
    XenosWindowServerTest | ../gpu-cgl-discovery/tiger-ssh "manifest='$manifest'
$remote"
