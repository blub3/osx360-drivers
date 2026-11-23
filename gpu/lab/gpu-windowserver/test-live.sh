#!/bin/sh
set -eu
set -o pipefail
cd "$(dirname "$0")"
scratch=$(mktemp -d /tmp/xenos-windowserver-run.XXXXXXXX)
trap 'rm -rf "$scratch"' 0
cp build/XenosWindowServerCheck RunXenosWindowServer.sh "$scratch/"
COPYFILE_DISABLE=1 tar --no-xattrs --no-mac-metadata -C "$scratch" -cf - \
    XenosWindowServerCheck RunXenosWindowServer.sh |
    ../gpu-cgl-discovery/tiger-ssh '
set -eu
export LC_ALL=C
work=$(mktemp -d /tmp/xenos-windowserver-run.XXXXXXXX)
trap '\''rm -rf "$work"'\'' 0
tar -xmf - -C "$work"
/bin/sh "$work/RunXenosWindowServer.sh" --baseline
/bin/sh "$work/RunXenosWindowServer.sh" --control
'
