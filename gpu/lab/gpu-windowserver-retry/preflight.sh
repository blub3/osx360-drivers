#!/bin/sh
set -eu
set -o pipefail
cd "$(dirname "$0")"
COPYFILE_DISABLE=1 tar --no-xattrs --no-mac-metadata -C link-preflight -cf - "$@" |
../gpu-cgl-discovery/tiger-ssh '
set -eu
export LC_ALL=C
scratch=$(mktemp -d /tmp/xenos-kext-preflight.XXXXXXXX)
trap '\''rm -rf "$scratch"'\'' 0
tar -xmf - -C "$scratch"
cd "$scratch"
addresses=$(/usr/sbin/kextstat)
addr() { printf "%s\n" "$addresses" | awk -v bundle="$1" '\''$6 == bundle {print $3; exit}'\''; }
pci=$(addr com.apple.iokit.IOPCIFamily)
graphics=$(addr com.apple.iokit.IOGraphicsFamily)
xenos=$(addr fish.goldfish64.osx360.XenonCoreGraphics)
test -n "$pci" && test -n "$graphics" && test -n "$xenos"
for variant in ./*; do
    test -d "$variant" || continue
    mkdir "$variant/symbols"
    printf "Testing %s without loading it\n" "$variant"
    rc=0
    /sbin/kextload -n -t -z -k /mach_kernel -s "$variant/symbols" \
        -a "com.apple.iokit.IOPCIFamily@$pci" -a "com.apple.iokit.IOGraphicsFamily@$graphics" \
        -a "fish.goldfish64.osx360.XenonCoreGraphics@$xenos" \
        "$variant/XenonCoreGraphics.kext" > "$variant/result" 2>&1 || rc=$?
    cat "$variant/result"
    ls -l "$variant/symbols"
    printf "kextload exit status %s\n" "$rc"
done
'
