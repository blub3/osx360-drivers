#!/bin/sh
set -eu
cd "$(dirname "$0")"
tc=../tx-build/toolchain/usr
discovery=../gpu-cgl-discovery
ref=$discovery/reference
driver=source/XenonCoreGraphics/src
mkdir -p build
"$tc/bin/powerpc-apple-darwin10-gcc-4.2.1" -B"$tc/libexec/gcc/darwin/ppc/" \
    -mmacosx-version-min=10.4 -std=c99 -O2 -Wall -Wextra -Werror \
    -I"$discovery" -I../gpu-cgl-offscreen -I"$driver" -c XenosDisplaySurfaceCheck.c -o build/XenosDisplaySurfaceCheck.o
"$tc/bin/ld" -arch ppc -macosx_version_min 10.4 -no_implicit_dylibs \
    -syslibroot "$ref/tiger-root" -e start -o build/XenosDisplaySurfaceCheck \
    "$ref/crt1.o" build/XenosDisplaySurfaceCheck.o "$discovery/build/XenosDisplay.o" "$ref/lib/libSystem.B.dylib"
