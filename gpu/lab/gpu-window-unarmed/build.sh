#!/bin/sh
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
lab=$(dirname "$here")
tc=${OSX360_TOOLCHAIN:-$lab/tx-build/toolchain/usr}
ref=${OSX360_TIGER_REFERENCE:-$lab/gpu-cgl-discovery/reference}
build=${1:?usage: build.sh output-directory [--overlap]}
overlap=
plugin=XenosWinAuto
control=CheckUnarmedOffscreen
case ${2:-} in
    --overlap) overlap=-DXENOS_UNARMED_OVERLAP; plugin=XenosWinOverlap; control=CheckUnarmedOverlap ;;
    '') ;;
    *) exit 2 ;;
esac
mkdir -p "$build"
tile=$lab/gpu-window-tile-capture
display=$lab/gpu-cgl-discovery

compile() {
    "$tc/bin/powerpc-apple-darwin10-gcc-4.2.1" -B"$tc/libexec/gcc/darwin/ppc/" \
        -mmacosx-version-min=10.4 -std=c99 -O2 -Wall -Wextra -Werror -DXENOS_UNARMED_BLIT ${overlap:+"$overlap"} \
        -I"$tile" -I"$display" -I"$lab/gpu-cgl-offscreen" -I"$lab/gpu-cgl-window" \
        -I"$tile/source/XenonCoreGraphics/src" -I"$tile/source/include" \
        -c "$1" -o "$build/${1##*/}.o"
}
for source in XenosWinTile XenosWindowCopy XenosBackend XenosWindowRepaintCheck XenosWindowPluginCheck; do
    compile "$tile/$source.c"
done
compile "$display/XenosDisplay.c"
compile "$here/$control.c"

link() {
    "$tc/bin/ld" -arch ppc -macosx_version_min 10.4 -no_implicit_dylibs \
        -syslibroot "$ref/tiger-root" "$@" "$ref/lib/libSystem.B.dylib" \
        "$tc/lib/gcc/powerpc-apple-darwin10/4.2.1/libgcc.a"
}
link -bundle -o "$build/$plugin" "$ref/bundle1.o" \
    "$build/XenosWinTile.c.o" "$build/XenosWindowCopy.c.o" "$build/XenosBackend.c.o"
link -e start -o "$build/XenosWindowRepaintCheck" "$ref/crt1.o" \
    "$build/XenosWindowRepaintCheck.c.o" "$build/XenosWindowCopy.c.o" \
    "$build/XenosBackend.c.o" "$build/XenosDisplay.c.o"
link -e start -o "$build/XenosWindowPluginCheck" "$ref/crt1.o" \
    "$build/XenosWindowPluginCheck.c.o"
link -e start -o "$build/$control" "$ref/crt1.o" \
    "$build/$control.c.o" "$build/XenosWindowCopy.c.o" "$build/XenosBackend.c.o"
