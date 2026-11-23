#!/bin/sh
set -eu
here=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
tc=$here/../tx-build/toolchain/usr
flags="-B$tc/libexec/gcc/darwin/ppc/ -B$tc/bin/"
for driver in XenonCoreGraphics XenonFramebufferTest; do
    cd "$here/source/$driver"
    sources=
    for source in src/*.cpp; do
        sources="$sources build_tiger/${source%.cpp}.o"
    done
    make -j4 OSX_VERSION=tiger DARLING_SHELL= \
        CC="$tc/bin/powerpc-apple-darwin10-gcc-4.2.1 $flags" \
        CXX="$tc/bin/powerpc-apple-darwin10-g++-4.2.1 $flags" \
        $sources "build_tiger/build_tiger/$driver-info.o" "build_tiger/$driver-Info.plist"
done
cd "$here/source"
COPYFILE_DISABLE=1 tar --no-xattrs --no-mac-metadata -cf "$here/link-input.tar" \
    XenonCoreGraphics/build_tiger/build_tiger/*.o XenonCoreGraphics/build_tiger/src/*.o \
    XenonFramebufferTest/build_tiger/build_tiger/*.o XenonFramebufferTest/build_tiger/src/*.o
