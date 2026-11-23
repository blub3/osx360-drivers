#!/bin/sh
set -eu
umask 077
export LC_ALL=C
unset GL_RESOURCES XENOS_CGL_DISCOVERY
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
scratch=$(mktemp -d /tmp/xenos-cgl-discovery.XXXXXXXX)
trap 'rm -rf "$scratch"' 0
trap 'exit 1' HUP INT TERM

resources=$scratch/Resources
stock=/System/Library/Frameworks/OpenGL.framework/Versions/A/Resources
mkdir "$resources" "$resources/GLDriverXenos.bundle"
for item in "$stock"/*; do
    ln -s "$item" "$resources/"
done
cp "$here/GLDriverXenos" "$resources/GLDriverXenos.bundle/"
cp "$here/XenosCGLDiscovery" "$scratch/"
chmod 755 "$scratch/XenosCGLDiscovery"

"$scratch/XenosCGLDiscovery" > "$scratch/baseline" || {
    cat "$scratch/baseline"
    exit 1
}
cat "$scratch/baseline"
GL_RESOURCES="$resources/" XENOS_CGL_DISCOVERY=enabled \
    "$scratch/XenosCGLDiscovery" > "$scratch/private" || {
    cat "$scratch/private"
    exit 1
}
cat "$scratch/private"

# Compare the stock renderer records, disregarding their list positions.
awk '/^renderer\[/ { if ($2 != "id=0x0002e900") print $2, $3, $4 }' \
    "$scratch/baseline" | sort > "$scratch/stock-before"
awk '/^renderer\[/ { if ($2 != "id=0x0002e900") print $2, $3, $4 }' \
    "$scratch/private" | sort > "$scratch/stock-with-xenos"
cmp "$scratch/stock-before" "$scratch/stock-with-xenos"
"$scratch/XenosCGLDiscovery" > "$scratch/restored" || {
    cat "$scratch/restored"
    exit 1
}
cmp "$scratch/baseline" "$scratch/restored"
echo "XenosCGLDiscovery PASS stock renderers preserved and baseline restored"
