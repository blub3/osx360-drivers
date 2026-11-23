#!/bin/sh
set -eu
cd "$(dirname "$0")"
tc=../tx-build/toolchain/usr
ref=../gpu-cgl-discovery/reference
discovery=../gpu-cgl-discovery
offscreen=../gpu-cgl-offscreen
driver=../gpu-windowserver/source/XenonCoreGraphics/src
mkdir -p build
for src in XenosNativeGLD XenosNativeGLDCheck; do
    "$tc/bin/powerpc-apple-darwin10-gcc-4.2.1" -B"$tc/libexec/gcc/darwin/ppc/" \
        -mmacosx-version-min=10.4 -std=c99 -O2 -Wall -Wextra -Werror \
        -I"$discovery" -I"$offscreen" -I"$driver" -c "$src.c" -o "build/$src.o"
done
"$tc/bin/ld" -arch ppc -macosx_version_min 10.4 -no_implicit_dylibs \
    -syslibroot "$ref/tiger-root" -bundle -o build/GLDriverXenosNative \
    "$ref/bundle1.o" build/XenosNativeGLD.o "$offscreen/build/XenosBackend.o" \
    "$discovery/build/XenosDisplay.o" "$ref/lib/libSystem.B.dylib"
"$tc/bin/ld" -arch ppc -macosx_version_min 10.4 -no_implicit_dylibs \
    -syslibroot "$ref/tiger-root" -e start -o build/XenosNativeGLDCheck \
    "$ref/crt1.o" build/XenosNativeGLDCheck.o "$discovery/build/XenosDisplay.o" \
    "$ref/lib/libSystem.B.dylib"
