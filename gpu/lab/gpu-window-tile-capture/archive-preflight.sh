#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
../gpu-cgl-discovery/tiger-ssh '
set -eu
export LC_ALL=C
boot=/Volumes/OSX360BOOT
cd "$boot"
test "$(md5 -q XWRB.MKE)" = 035a699f914d23541fa2709107f065dd
scratch=$(mktemp -d /tmp/xenos-window-tile-audit.XXXXXXXX)
trap '\''rm -rf "$scratch"'\'' 0
cat > "$scratch/XWTC.MKE"
test "$(md5 -q "$scratch/XWTC.MKE")" = c36e84a7e31782ea14a20b3a931881fb
mkdir "$scratch/good" "$scratch/fix" "$scratch/symbols"
/usr/sbin/mkextunpack -v -a ppc -d "$scratch/good" "$boot/XWRB.MKE" > "$scratch/good.list"
/usr/sbin/mkextunpack -v -a ppc -d "$scratch/fix" "$scratch/XWTC.MKE" > "$scratch/fix.list"
cat "$scratch/fix.list"
sed "/XenonCoreGraphics/s/0.0.5d47/0.0.5d48/" "$scratch/good.list" > "$scratch/expected.list"
cmp "$scratch/expected.list" "$scratch/fix.list"
n=0
for directory in "$scratch/good"/*.kext; do
    name=${directory##*/}
    if [ "$name" = XenonCoreGraphics.kext ]; then
        continue
    fi
    cmp "$directory/Contents/MacOS/${name%.kext}" "$scratch/fix/$name/Contents/MacOS/${name%.kext}"
    n=$((n + 1))
done
test "$n" -eq 8
echo "Tiger verified eight byte-identical kext executables."
for name in XenonCoreGraphics XenonFramebufferTest; do
    info="$scratch/fix/$name.kext/Contents/Info.plist"
    /usr/bin/plutil -lint "$info"
    grep -q "GLDriverXenosCompositorRetry" "$info"
done
grep -Fq XenosWinTile.plugin "$scratch/fix/XenonFramebufferTest.kext/Contents/Info.plist"
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
' < boot-stage/XWTC.MKE
