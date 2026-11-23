#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
COPYFILE_DISABLE=1 tar --no-xattrs --no-mac-metadata -C link-preflight -cf -     XenonCoreGraphics.kext XenonFramebufferTest.kext |
../gpu-cgl-discovery/tiger-ssh '
set -eu
export LC_ALL=C
scratch=$(mktemp -d /tmp/xenos-compositor-preflight.XXXXXXXX)
trap '\''rm -rf "$scratch"'\'' 0
tar -xmf - -C "$scratch"
cd "$scratch"
mkdir symbols
addresses=$(/usr/sbin/kextstat)
addr() { printf "%s\n" "$addresses" | awk -v bundle="$1" '\''$6 == bundle {print $3; exit}'\''; }
pci=$(addr com.apple.iokit.IOPCIFamily)
graphics=$(addr com.apple.iokit.IOGraphicsFamily)
for name in XenonCoreGraphics XenonFramebufferTest; do
    bundle="fish.goldfish64.osx360.$name"
    address=$(addr "$bundle")
    test -n "$pci" && test -n "$graphics" && test -n "$address"
    /sbin/kextload -n -t -z -k /mach_kernel -s symbols \
        -a "com.apple.iokit.IOPCIFamily@$pci" -a "com.apple.iokit.IOGraphicsFamily@$graphics" \
        -a "$bundle@$address" "$name.kext"
    test -s "symbols/$bundle.sym"
    printf "%s offline link PASS\n" "$name"
done
'
