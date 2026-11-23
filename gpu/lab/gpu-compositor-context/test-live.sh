#!/bin/sh
set -eu
set -o pipefail
cd "$(dirname "$0")"
export LC_ALL=C
mkdir -p build/smoke/GLDriverXenosCompositor.bundle/Contents/MacOS
cp build/GLDriverXenosCompositor build/smoke/GLDriverXenosCompositor.bundle/Contents/MacOS/
cp build/XenosCompositorContextCheck RunXenosCompositorContext.sh build/smoke/
COPYFILE_DISABLE=1 tar --no-xattrs --no-mac-metadata -C build/smoke -cf - . |
../gpu-cgl-discovery/tiger-ssh '
set -eu
export LC_ALL=C
d=$(mktemp -d /tmp/xenos-compositor-smoke.XXXXXXXX)
trap "rm -rf \"$d\"" 0
tar -xmf - -C "$d"
/bin/sh "$d/RunXenosCompositorContext.sh"
'
