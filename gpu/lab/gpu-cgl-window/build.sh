#!/bin/sh
set -eu
cd "$(dirname "$0")"
tc=../tx-build/toolchain/usr
reference=../gpu-cgl-discovery/reference
offscreen=../gpu-cgl-offscreen
mkdir -p build
for src in XenosWindowGLI XenosPresentation XenosCGLWindow; do
    "$tc/bin/powerpc-apple-darwin10-gcc-4.2.1" -B"$tc/libexec/gcc/darwin/ppc/" \
        -mmacosx-version-min=10.4 -std=c99 -O2 -Wall -Wextra -Werror \
        -I. -I"$offscreen" -I../gpu-cgl-discovery -I"$offscreen/source/XenonCoreGraphics/src" \
        -c "$src.c" -o "build/$src.o"
done
"$tc/bin/ld" -arch ppc -macosx_version_min 10.4 -no_implicit_dylibs \
    -syslibroot "$reference/tiger-root" -bundle -o build/GLEngineXenosWindow \
    "$reference/bundle1.o" build/XenosWindowGLI.o build/XenosPresentation.o \
    "$offscreen/build/XenosBackend.o" ../gpu-cgl-discovery/build/XenosDisplay.o \
    "$reference/lib/libSystem.B.dylib" "$tc/lib/gcc/powerpc-apple-darwin10/4.2.1/libgcc.a"
"$tc/bin/ld" -arch ppc -macosx_version_min 10.4 -no_implicit_dylibs \
    -syslibroot "$reference/tiger-root" -e start -o build/XenosCGLWindow \
    "$reference/crt1.o" build/XenosCGLWindow.o "$reference/lib/libSystem.B.dylib" \
    "$tc/lib/gcc/powerpc-apple-darwin10/4.2.1/libgcc.a"
