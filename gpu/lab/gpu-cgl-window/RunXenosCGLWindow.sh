#!/bin/sh
set -eu
umask 077
export LC_ALL=C
unset GL_RESOURCES
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
scratch=$(mktemp -d /tmp/xenos-cgl-window.XXXXXXXX)
trap 'rm -rf "$scratch"' 0
trap 'exit 1' HUP INT TERM
resources=$scratch/Resources
stock=/System/Library/Frameworks/OpenGL.framework/Versions/A/Resources
mkdir "$resources" "$resources/GLEngineXenosWindow.bundle"
for item in "$stock"/*; do
    ln -s "$item" "$resources/"
done
cp "$here/GLEngineXenosWindow" "$resources/GLEngineXenosWindow.bundle/"
cp "$here/XenosCGLWindow" "$scratch/"
chmod 755 "$scratch/XenosCGLWindow"
GL_RESOURCES="$resources/" "$scratch/XenosCGLWindow"
