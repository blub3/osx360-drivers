#!/bin/sh
set -eu
umask 077
export LC_ALL=C
unset GL_RESOURCES XENOS_GLD_COMPOSITOR_TEST
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
scratch=$(mktemp -d /tmp/xenos-compositor-context.XXXXXXXX)
trap 'rm -rf "$scratch"' 0
trap 'exit 1' HUP INT TERM
resources=$scratch/Resources
name=GLDriverXenosCompositor
mkdir "$resources" "$resources/$name.bundle"
for item in /System/Library/Frameworks/OpenGL.framework/Resources/*; do
    ln -s "$item" "$resources/"
done
cp "$here/$name.bundle/Contents/MacOS/$name" "$resources/$name.bundle/"
cp "$here/XenosCompositorContextCheck" "$scratch/"
chmod 700 "$scratch/XenosCompositorContextCheck"
GL_RESOURCES="$resources/" XENOS_GLD_COMPOSITOR_TEST=1 "$scratch/XenosCompositorContextCheck"
