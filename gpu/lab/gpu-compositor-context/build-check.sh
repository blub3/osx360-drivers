#!/bin/sh
set -eu
cd "$(dirname "$0")"
tc=../tx-build/toolchain/usr
ref=../gpu-cgl-discovery/reference
driver=../gpu-windowserver-submit-retry/source/XenonCoreGraphics/src
"$tc/bin/powerpc-apple-darwin10-gcc-4.2.1" -B"$tc/libexec/gcc/darwin/ppc/" \
    -mmacosx-version-min=10.4 -std=c99 -O2 -Wall -Wextra -Werror \
    -I../gpu-cgl-discovery -I../gpu-cgl-offscreen -I"$driver" \
    -c XenosCompositorContextCheck.c -o build/XenosCompositorContextCheck.o
"$tc/bin/ld" -arch ppc -macosx_version_min 10.4 -no_implicit_dylibs -syslibroot "$ref/tiger-root" -e start \
    -o build/XenosCompositorContextCheck "$ref/crt1.o" build/XenosCompositorContextCheck.o \
    "$ref/lib/libSystem.B.dylib"
