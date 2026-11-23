#!/bin/sh
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
tc=$here/../tx-build/toolchain/usr
flags="-B$tc/libexec/gcc/darwin/ppc/ -B$tc/bin/"
cd "$here/source/XenonCoreGraphics"
make -j4 OSX_VERSION=tiger DARLING_SHELL= \
    CC="$tc/bin/powerpc-apple-darwin10-gcc-4.2.1 $flags" \
    CXX="$tc/bin/powerpc-apple-darwin10-g++-4.2.1 $flags" \
    build_tiger/src/XenosController.o build_tiger/src/XenosController_Firmware.o \
    build_tiger/src/XenosController_Setup.o build_tiger/src/XenosQuadClient.o build_tiger/src/XenosDisplayAccelerator.o \
    build_tiger/build_tiger/XenonCoreGraphics-info.o build_tiger/XenonCoreGraphics-Info.plist
COPYFILE_DISABLE=1 tar --no-xattrs --no-mac-metadata -C build_tiger -cf "$here/link-input.tar" \
    build_tiger/XenonCoreGraphics-info.o src/XenosController.o src/XenosController_Firmware.o \
    src/XenosController_Setup.o src/XenosQuadClient.o src/XenosDisplayAccelerator.o
