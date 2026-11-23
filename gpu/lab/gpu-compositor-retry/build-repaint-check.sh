#!/bin/sh
set -eu
cd "$(dirname "$0")"
tc=../tx-build/toolchain/usr
discovery=../gpu-cgl-discovery
ref=$discovery/reference
offscreen=../gpu-cgl-offscreen
driver=../gpu-compositor-context/source/XenonCoreGraphics/src
mkdir -p build
"$tc/bin/powerpc-apple-darwin10-gcc-4.2.1" -B"$tc/libexec/gcc/darwin/ppc/" \
    -mmacosx-version-min=10.4 -std=c99 -O2 -Wall -Wextra -Werror \
    -I"$discovery" -I"$offscreen" -I"$driver" -I../gpu-compositor-context/source/include -I../gpu-cgl-window \
    -c XenosCompositorRepaintCheck.c -o build/XenosCompositorRepaintCheck.o
"$tc/bin/ld" -arch ppc -macosx_version_min 10.4 -no_implicit_dylibs \
    -syslibroot "$ref/tiger-root" -e start -o build/XenosCompositorRepaintCheck \
    "$ref/crt1.o" build/XenosCompositorRepaintCheck.o "$offscreen/build/XenosBackend.o" \
    "$discovery/build/XenosDisplay.o" "$ref/lib/libSystem.B.dylib" \
    "$tc/lib/gcc/powerpc-apple-darwin10/4.2.1/libgcc.a"
