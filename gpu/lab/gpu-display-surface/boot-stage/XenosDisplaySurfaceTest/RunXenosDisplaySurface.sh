#!/bin/sh
set -eu
export LC_ALL=C
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
case "${1:---probe}" in
    --baseline) expected=0.0.5d41 ;;
    --probe) expected=0.0.5d42 ;;
    *) echo "Usage: $0 [--baseline|--probe]" >&2; exit 2 ;;
esac
/usr/sbin/kextstat | grep -F "XenonCoreGraphics ($expected)"
scratch=$(mktemp -d /tmp/xenos-display-surface.XXXXXXXX)
trap 'rm -rf "$scratch"' 0
trap 'exit 1' HUP INT TERM
cp "$here/XenosDisplaySurfaceCheck" "$scratch/"
chmod 700 "$scratch/XenosDisplaySurfaceCheck"
"$scratch/XenosDisplaySurfaceCheck" "${1:---probe}"
