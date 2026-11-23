#!/bin/sh
set -eu
cd "$(dirname "$0")"
tc=../tx-build/toolchain/usr
ref=../gpu-cgl-discovery/reference
driver=../gpu-compositor-context/source/XenonCoreGraphics/src
mkdir -p build
for name in XenosBeamBlit XenosFramebufferBlitCheck; do
    "$tc/bin/powerpc-apple-darwin10-gcc-4.2.1" -B"$tc/libexec/gcc/darwin/ppc/" \
        -mmacosx-version-min=10.4 -std=c99 -O2 -Wall -Wextra -Werror \
        -I../gpu-cgl-discovery -I../gpu-cgl-offscreen -I"$driver" -c "$name.c" -o "build/$name.o"
done
"$tc/bin/ld" -arch ppc -macosx_version_min 10.4 -no_implicit_dylibs -syslibroot "$ref/tiger-root" \
    -bundle -o build/XenosBeamBlit "$ref/bundle1.o" build/XenosBeamBlit.o "$ref/lib/libSystem.B.dylib"
"$tc/bin/ld" -arch ppc -macosx_version_min 10.4 -no_implicit_dylibs -syslibroot "$ref/tiger-root" -e start \
    -o build/XenosFramebufferBlitCheck "$ref/crt1.o" build/XenosFramebufferBlitCheck.o "$ref/lib/libSystem.B.dylib"
