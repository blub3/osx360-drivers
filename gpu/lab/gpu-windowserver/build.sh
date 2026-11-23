#!/bin/sh
set -eu
cd "$(dirname "$0")"
tc=../tx-build/toolchain/usr
ref=../gpu-cgl-discovery/reference
offscreen=../gpu-cgl-offscreen
mkdir -p build
"$tc/bin/powerpc-apple-darwin10-gcc-4.2.1" -B"$tc/libexec/gcc/darwin/ppc/" \
    -mmacosx-version-min=10.4 -std=c99 -O2 -Wall -Wextra -Werror \
    -I"$offscreen" -I../gpu-cgl-window -I../gpu-cgl-discovery -Isource/XenonCoreGraphics/src \
    -c XenosWindowServerCheck.c -o build/XenosWindowServerCheck.o
"$tc/bin/ld" -arch ppc -macosx_version_min 10.4 -no_implicit_dylibs \
    -syslibroot "$ref/tiger-root" -e start -o build/XenosWindowServerCheck \
    "$ref/crt1.o" build/XenosWindowServerCheck.o "$offscreen/build/XenosBackend.o" \
    ../gpu-cgl-discovery/build/XenosDisplay.o "$ref/lib/libSystem.B.dylib" \
    "$tc/lib/gcc/powerpc-apple-darwin10/4.2.1/libgcc.a"
