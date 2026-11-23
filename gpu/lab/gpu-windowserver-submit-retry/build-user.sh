#!/bin/sh
set -eu
cd "$(dirname "$0")"
tc=../tx-build/toolchain/usr
discovery=../gpu-cgl-discovery
ref=$discovery/reference
offscreen=../gpu-cgl-offscreen
driver=source/XenonCoreGraphics/src
mkdir -p build
"$tc/bin/powerpc-apple-darwin10-gcc-4.2.1" -B"$tc/libexec/gcc/darwin/ppc/" \
    -mmacosx-version-min=10.4 -std=c99 -O2 -Wall -Wextra -Werror \
    -I"$discovery" -I"$offscreen" -I"$driver" -Isource/include -I../gpu-cgl-window \
    -c XenosWindowServerFramebufferCheck.c -o build/XenosWindowServerFramebufferCheck.o
"$tc/bin/ld" -arch ppc -macosx_version_min 10.4 -no_implicit_dylibs \
    -syslibroot "$ref/tiger-root" -e start -o build/XenosWindowServerFramebufferCheck \
    "$ref/crt1.o" build/XenosWindowServerFramebufferCheck.o "$offscreen/build/XenosBackend.o" \
    "$discovery/build/XenosDisplay.o" "$ref/lib/libSystem.B.dylib" \
    "$tc/lib/gcc/powerpc-apple-darwin10/4.2.1/libgcc.a"
