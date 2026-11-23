#!/bin/sh
set -eu
set -o pipefail
cd "$(dirname "$0")"
../gpu-cgl-discovery/tiger-ssh '/usr/sbin/kextstat | grep -F XenonCoreGraphics'
scratch=$(mktemp -d /tmp/xenos-offscreen-transfer.XXXXXXXX)
trap 'rm -rf "$scratch"' 0
cp build/GLEngineXenos build/XenosCGLOffscreen RunXenosCGLOffscreen.sh "$scratch/"
COPYFILE_DISABLE=1 tar --no-xattrs --no-mac-metadata -C "$scratch" -cf - \
    GLEngineXenos XenosCGLOffscreen RunXenosCGLOffscreen.sh |
    ../gpu-cgl-discovery/tiger-ssh '/bin/sh -c '\''
        set -eu
        export LC_ALL=C
        work=$(mktemp -d /tmp/xenos-offscreen-transfer.XXXXXXXX)
        trap "rm -rf \"$work\"" 0
        tar -xmf - -C "$work"
        /bin/sh "$work/RunXenosCGLOffscreen.sh" --legacy-fence-only
    '\'''
