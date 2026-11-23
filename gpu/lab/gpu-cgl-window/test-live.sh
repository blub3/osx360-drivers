#!/bin/sh
set -eu
set -o pipefail
cd "$(dirname "$0")"
scratch=$(mktemp -d /tmp/xenos-window-local.XXXXXXXX)
trap 'rm -rf "$scratch"' 0
cp build/GLEngineXenosWindow build/XenosCGLWindow RunXenosCGLWindow.sh "$scratch/"
COPYFILE_DISABLE=1 tar --no-xattrs --no-mac-metadata -C "$scratch" -cf - \
    GLEngineXenosWindow XenosCGLWindow RunXenosCGLWindow.sh |
    ../gpu-cgl-discovery/tiger-ssh '/bin/sh -c '\''
        set -eu
        export LC_ALL=C
        work=$(mktemp -d /tmp/xenos-window-run.XXXXXXXX)
        trap "rm -rf \"$work\"" 0
        tar -xmf - -C "$work"
        /bin/sh "$work/RunXenosCGLWindow.sh"
    '\'''
