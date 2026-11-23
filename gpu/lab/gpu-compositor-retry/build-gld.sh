#!/bin/sh
set -eu
cd "$(dirname "$0")"
tc=../tx-build/toolchain/usr
ref=../gpu-cgl-discovery/reference
driver=../gpu-windowserver-submit-retry/source/XenonCoreGraphics/src
mkdir -p build
python3 generate-delegates.py
"$tc/bin/powerpc-apple-darwin10-gcc-4.2.1" -B"$tc/libexec/gcc/darwin/ppc/" \
    -mmacosx-version-min=10.4 -std=c99 -O2 -Wall -Wextra -Werror \
    -I../gpu-cgl-discovery -I../gpu-cgl-offscreen -I"$driver" -c XenosCompositorGLD.c -o build/XenosCompositorGLD.o
"$tc/bin/powerpc-apple-darwin10-gcc-4.2.1" -B"$tc/libexec/gcc/darwin/ppc/" \
    -mmacosx-version-min=10.4 -c build/GLDDelegate.s -o build/GLDDelegate.o
"$tc/bin/ld" -arch ppc -macosx_version_min 10.4 -no_implicit_dylibs -syslibroot "$ref/tiger-root" \
    -bundle -o build/GLDriverXenosCompositorRetry "$ref/bundle1.o" build/XenosCompositorGLD.o build/GLDDelegate.o \
    ../gpu-cgl-offscreen/build/XenosBackend.o "$ref/lib/libSystem.B.dylib"
