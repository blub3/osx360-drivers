#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
../gpu-cgl-discovery/tiger-ssh '
set -eu
export LC_ALL=C
boot=/Volumes/OSX360BOOT
cd "$boot"
test "$(md5 -q XWRR.MKE)" = 8de0a6ef81b355802c4a6d6a313ec593
scratch=$(mktemp -d /tmp/xenos-window-map-audit.XXXXXXXX)
trap '\''rm -rf "$scratch"'\'' 0
cat > "$scratch/XWFM.MKE"
test "$(md5 -q "$scratch/XWFM.MKE")" = 1b39f90c3118bd04aca9e378a10806e5
mkdir "$scratch/good" "$scratch/fix" "$scratch/symbols"
/usr/sbin/mkextunpack -v -a ppc -d "$scratch/good" "$boot/XWRR.MKE" > "$scratch/good.list"
/usr/sbin/mkextunpack -v -a ppc -d "$scratch/fix" "$scratch/XWFM.MKE" > "$scratch/fix.list"
cat "$scratch/fix.list"
sed "s/XenonFramebufferTest (0.0.5d2)/XenonFramebufferTest (0.0.5d3)/" "$scratch/good.list" > "$scratch/expected"
cmp "$scratch/expected" "$scratch/fix.list"
n=0
for directory in "$scratch/good"/*.kext; do
    name=${directory##*/}
    if [ "$name" = XenonFramebufferTest.kext ]; then
        test "$(md5 -q "$scratch/fix/$name/Contents/MacOS/XenonFramebufferTest")" = 618920f42fbd4dba53edd907f72fae93
        continue
    fi
    cmp "$directory/Contents/MacOS/${name%.kext}" "$scratch/fix/$name/Contents/MacOS/${name%.kext}"
    n=$((n + 1))
done
test "$n" -eq 8
echo "Tiger unpacked the new framebuffer executable and verified eight byte-identical existing executables."
for name in XenonCoreGraphics XenonFramebufferTest; do
    info="$scratch/fix/$name.kext/Contents/Info.plist"
    /usr/bin/plutil -lint "$info"
    grep -q "GLDriverXenosCompositorRetry" "$info"
done
grep -Fq XenosWinMap.plugin "$scratch/fix/XenonFramebufferTest.kext/Contents/Info.plist"
addresses=$(/usr/sbin/kextstat)
addr() { printf "%s\n" "$addresses" | awk -v bundle="$1" '\''$6 == bundle {print $3; exit}'\''; }
pci=$(addr com.apple.iokit.IOPCIFamily)
graphics=$(addr com.apple.iokit.IOGraphicsFamily)
test -n "$pci" && test -n "$graphics"
for name in XenonCoreGraphics XenonFramebufferTest; do
    bundle="fish.goldfish64.osx360.$name"
    address=$(addr "$bundle")
    test -n "$address"
    /sbin/kextload -n -t -z -k /mach_kernel -s "$scratch/symbols" \
        -a "com.apple.iokit.IOPCIFamily@$pci" -a "com.apple.iokit.IOGraphicsFamily@$graphics" \
        -a "$bundle@$address" "$scratch/fix/$name.kext"
    test -s "$scratch/symbols/$bundle.sym"
    echo "$name offline link PASS"
done
' < boot-stage/XWFM.MKE
