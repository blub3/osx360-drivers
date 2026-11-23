#!/bin/sh
set -eu
set -o pipefail
cd "$(dirname "$0")"
export LC_ALL=C
manifest=$(cd boot-stage && find . -type f -exec md5 -r {} \; | sort)
{
    printf "manifest='%s'\n" "$manifest"
    cat stage-remote.sh
} > stage-command.sh
COPYFILE_DISABLE=1 tar --no-xattrs --no-mac-metadata -C boot-stage -cf - \
    XenosWFR.mkext openbios-xbox360-gpu-windowserver-framebuffer.elf kboot-gpu-windowserver-framebuffer.conf XenosWindowServerFramebufferTest |
    ../gpu-cgl-discovery/tiger-ssh "$(cat stage-command.sh)"
