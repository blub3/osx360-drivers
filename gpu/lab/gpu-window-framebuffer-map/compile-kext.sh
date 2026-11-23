#!/bin/sh
set -eu
here=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
tc=$here/../tx-build/toolchain/usr
flags="-B$tc/libexec/gcc/darwin/ppc/ -B$tc/bin/"
cd "$here/source/XenonFramebufferTest"
make -j2 OSX_VERSION=tiger DARLING_SHELL= \
    CC="$tc/bin/powerpc-apple-darwin10-gcc-4.2.1 $flags" \
    CXX="$tc/bin/powerpc-apple-darwin10-g++-4.2.1 $flags" \
    build_tiger/src/XenonFramebufferTest.o build_tiger/build_tiger/XenonFramebufferTest-info.o
cd "$here/source"
COPYFILE_DISABLE=1 tar --no-xattrs --no-mac-metadata -cf "$here/link-input.tar" \
    XenonFramebufferTest/build_tiger/build_tiger/*.o XenonFramebufferTest/build_tiger/src/*.o
