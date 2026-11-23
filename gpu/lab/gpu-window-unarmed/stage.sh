#!/bin/bash
set -euo pipefail
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
stage=${1:?usage: stage.sh staging-directory local-volume-or-ssh-wrapper}
target=${2:?usage: stage.sh staging-directory local-volume-or-ssh-wrapper [--overlap]}
export LC_ALL=C COPYFILE_DISABLE=1
case ${3:-} in
    --overlap)
        variant=overlap
        set -- XWOV.MKE WINOVR.ELF kboot-gpu-window-unarmed-overlap.conf \
            kboot.conf.before-gpuwinoverlap-stage.bak XenosWindowOverlapTest ;;
    '')
        variant=single
        set -- XWAU.MKE WINAUTO.ELF kboot-gpu-window-unarmed.conf \
            kboot.conf.before-gpuwinauto-stage.bak XenosWindowUnarmedTest ;;
    *) exit 2 ;;
esac
manifest=$(cd "$stage" && find . -type f -exec md5 -r {} \; | sort)
case "$manifest" in *"'"*) exit 2 ;; esac
remote=$(printf "variant='%s'\nmanifest='%s'\n" "$variant" "$manifest"; cat "$here/stage-remote.sh")
tar --no-xattrs --no-mac-metadata -C "$stage" -cf - "$@" |
    if [ -d "$target" ]; then
        BOOT_VOLUME="$target" /bin/sh -c "$remote"
    else
        "$target" "$remote"
    fi
