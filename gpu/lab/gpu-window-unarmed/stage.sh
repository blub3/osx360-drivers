#!/bin/bash
set -euo pipefail
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
stage=${1:?usage: stage.sh staging-directory local-volume-or-ssh-wrapper}
target=${2:?usage: stage.sh staging-directory local-volume-or-ssh-wrapper}
export LC_ALL=C COPYFILE_DISABLE=1
manifest=$(cd "$stage" && find . -type f -exec md5 -r {} \; | sort)
remote=$(printf "manifest='%s'\n" "$manifest"; cat "$here/stage-remote.sh")
tar --no-xattrs --no-mac-metadata -C "$stage" -cf - \
    XWAU.MKE WINAUTO.ELF kboot-gpu-window-unarmed.conf \
    kboot.conf.before-gpuwinauto-stage.bak XenosWindowUnarmedTest |
    if [ -d "$target" ]; then
        BOOT_VOLUME="$target" /bin/sh -c "$remote"
    else
        "$target" "$remote"
    fi
