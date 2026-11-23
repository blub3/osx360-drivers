#!/bin/sh
set -eu
umask 077
export LC_ALL=C
unset GL_RESOURCES XENOS_CGL_LEGACY_FENCE_ONLY
case "${1-}" in
    "") ;;
    --legacy-fence-only) XENOS_CGL_LEGACY_FENCE_ONLY=1; export XENOS_CGL_LEGACY_FENCE_ONLY ;;
    *) echo "Usage: $0 [--legacy-fence-only]" >&2; exit 2 ;;
esac
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
scratch=$(mktemp -d /tmp/xenos-cgl-offscreen.XXXXXXXX)
trap 'rm -rf "$scratch"' 0
trap 'exit 1' HUP INT TERM
resources=$scratch/Resources
stock=/System/Library/Frameworks/OpenGL.framework/Versions/A/Resources
mkdir "$resources" "$resources/GLEngineXenos.bundle"
for item in "$stock"/*; do
    ln -s "$item" "$resources/"
done
cp "$here/GLEngineXenos" "$resources/GLEngineXenos.bundle/"
cp "$here/XenosCGLOffscreen" "$scratch/"
chmod 755 "$scratch/XenosCGLOffscreen"
GL_RESOURCES="$resources/" "$scratch/XenosCGLOffscreen"
