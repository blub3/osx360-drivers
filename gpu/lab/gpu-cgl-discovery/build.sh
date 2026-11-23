#!/bin/sh
set -eu
cd "$(dirname "$0")"
toolchain=../tx-build/toolchain/usr/bin
gcc=$toolchain/powerpc-apple-darwin10-gcc-4.2.1
assembler=../tx-build/toolchain/usr/libexec/gcc/darwin/ppc/
ld=$toolchain/ld
mkdir -p build
for source in XenosDisplay XenosRenderer XenosCGLDiscovery; do
    "$gcc" -B"$assembler" -mmacosx-version-min=10.4 -std=c99 -O2 -Wall -Wextra -Werror \
        -c "$source.c" -o "build/$source.o"
done
"$ld" -arch ppc -macosx_version_min 10.4 -no_implicit_dylibs -syslibroot reference/tiger-root -bundle -o build/GLDriverXenos \
    reference/bundle1.o build/XenosRenderer.o build/XenosDisplay.o \
    reference/lib/libSystem.B.dylib
"$ld" -arch ppc -macosx_version_min 10.4 -no_implicit_dylibs -syslibroot reference/tiger-root -e start -o build/XenosCGLDiscovery \
    reference/crt1.o build/XenosCGLDiscovery.o build/XenosDisplay.o \
    reference/lib/libSystem.B.dylib
