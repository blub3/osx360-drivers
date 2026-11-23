#!/bin/sh
set -eu
cd "$(dirname "$0")"
tc=../tx-build/toolchain/usr
ref=../gpu-cgl-discovery/reference
driver=../gpu-compositor-context/source/XenonCoreGraphics/src
mkdir -p build
for name in XenosWinMap XenosWindowCopy XenosWindowRepaintCheck XenosWindowPluginCheck XenosBackend; do
    "$tc/bin/powerpc-apple-darwin10-gcc-4.2.1" -B"$tc/libexec/gcc/darwin/ppc/" \
        -mmacosx-version-min=10.4 -std=c99 -O2 -Wall -Wextra -Werror \
        -I../gpu-cgl-discovery -I../gpu-cgl-offscreen -I../gpu-cgl-window -I"$driver" -Isource/include \
        -c "$name.c" -o "build/$name.o"
done
"$tc/bin/ld" -arch ppc -macosx_version_min 10.4 -no_implicit_dylibs -syslibroot "$ref/tiger-root" \
    -bundle -o build/XenosWinMap "$ref/bundle1.o" build/XenosWinMap.o build/XenosWindowCopy.o \
    build/XenosBackend.o "$ref/lib/libSystem.B.dylib"
"$tc/bin/ld" -arch ppc -macosx_version_min 10.4 -no_implicit_dylibs -syslibroot "$ref/tiger-root" \
    -e start -o build/XenosWindowRepaintCheck "$ref/crt1.o" build/XenosWindowRepaintCheck.o \
    build/XenosWindowCopy.o build/XenosBackend.o ../gpu-cgl-discovery/build/XenosDisplay.o \
    "$ref/lib/libSystem.B.dylib" "$tc/lib/gcc/powerpc-apple-darwin10/4.2.1/libgcc.a"
"$tc/bin/ld" -arch ppc -macosx_version_min 10.4 -no_implicit_dylibs -syslibroot "$ref/tiger-root" \
    -e start -o build/XenosWindowPluginCheck "$ref/crt1.o" build/XenosWindowPluginCheck.o \
    "$ref/lib/libSystem.B.dylib"
