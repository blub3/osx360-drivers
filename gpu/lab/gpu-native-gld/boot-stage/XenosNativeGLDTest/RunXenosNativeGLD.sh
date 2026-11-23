#!/bin/sh
set -eu
umask 077
export LC_ALL=C
unset GL_RESOURCES XENOS_NATIVE_GLD_PROBE
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
scratch=$(mktemp -d /tmp/xenos-native-gld.XXXXXXXX)
trap 'rm -rf "$scratch"' 0
trap 'exit 1' HUP INT TERM
cp "$here/XenosNativeGLDCheck" "$scratch/"
chmod 755 "$scratch/XenosNativeGLDCheck"

case "${1:---private}" in
    --native)
        # Test the registry-driven lookup, with Tiger's default resource paths.
        "$scratch/XenosNativeGLDCheck" --probe
        ;;
    --private)
        resources=$scratch/Resources
        stock=/System/Library/Frameworks/OpenGL.framework/Versions/A/Resources
        bundle=GLDriverXenosNative.bundle
        mkdir "$resources" "$resources/$bundle"
        for item in "$stock"/*; do
            ln -s "$item" "$resources/"
        done
        cp "$here/$bundle/Contents/MacOS/GLDriverXenosNative" "$resources/$bundle/"
        "$scratch/XenosNativeGLDCheck" --baseline > "$scratch/baseline" || {
            cat "$scratch/baseline"
            exit 1
        }
        cat "$scratch/baseline"
        GL_RESOURCES="$resources/" "$scratch/XenosNativeGLDCheck" --probe > "$scratch/probe" || {
            cat "$scratch/probe"
            exit 1
        }
        cat "$scratch/probe"
        "$scratch/XenosNativeGLDCheck" --baseline > "$scratch/restored" || {
            cat "$scratch/restored"
            exit 1
        }
        awk '/^renderer /' "$scratch/baseline" | sort > "$scratch/stock-before"
        awk '/^renderer / && $2 != "id=0x0002e900"' "$scratch/probe" | sort > "$scratch/stock-with-xenos"
        awk '/^renderer /' "$scratch/restored" | sort > "$scratch/stock-after"
        cmp "$scratch/stock-before" "$scratch/stock-with-xenos"
        cmp "$scratch/stock-before" "$scratch/stock-after"
        echo "XenosNativeGLD PASS stock renderer list preserved"
        ;;
    *) echo "Usage: $0 [--private|--native]" >&2; exit 2 ;;
esac
