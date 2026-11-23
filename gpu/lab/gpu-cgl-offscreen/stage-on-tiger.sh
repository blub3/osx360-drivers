#!/bin/sh
set -eu
set -o pipefail
cd "$(dirname "$0")"
manifest=$(python3 package-offscreen.py)
remote=$(cat stage-remote.sh)
COPYFILE_DISABLE=1 tar --no-xattrs --no-mac-metadata -C boot-stage -cf - \
    XenosCGL.mkext openbios-xbox360-gpu-cgl-offscreen.elf kboot-gpu-cgl-offscreen.conf \
    XenosCGLOffscreenTest | ../gpu-cgl-discovery/tiger-ssh "manifest='$manifest'
$remote"
