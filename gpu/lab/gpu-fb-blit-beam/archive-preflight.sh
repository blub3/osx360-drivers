#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
../gpu-cgl-discovery/tiger-ssh '
set -eu
export LC_ALL=C
boot=/Volumes/OSX360BOOT
cd "$boot"
test "$(md5 -q XWCF.MKE)" = c603562c9452658c09f5e25db66ff936
scratch=$(mktemp -d /tmp/xenos-fb-blit-beam-audit.XXXXXXXX)
trap '\''rm -rf "$scratch"'\'' 0
cat > "$scratch/XWBM.MKE"
test "$(md5 -q "$scratch/XWBM.MKE")" = 320b36a69712ad437d044017d633d45c
mkdir "$scratch/good" "$scratch/fix" "$scratch/symbols"
/usr/sbin/mkextunpack -v -a ppc -d "$scratch/good" "$boot/XWCF.MKE" > "$scratch/good.list"
/usr/sbin/mkextunpack -v -a ppc -d "$scratch/fix" "$scratch/XWBM.MKE" > "$scratch/fix.list"
cat "$scratch/fix.list"
cmp "$scratch/good.list" "$scratch/fix.list"
n=0
for directory in "$scratch/good"/*.kext; do
    name=${directory##*/}
    cmp "$directory/Contents/MacOS/${name%.kext}" "$scratch/fix/$name/Contents/MacOS/${name%.kext}"
    n=$((n + 1))
done
test "$n" -eq 9
echo "All nine executables unpacked by Tiger and matched the working archive byte for byte."
for name in XenonCoreGraphics XenonFramebufferTest; do
    info="$scratch/fix/$name.kext/Contents/Info.plist"
    /usr/bin/plutil -lint "$info"
    grep -q "GLDriverXenosCompositorRetry" "$info"
done
grep -Fq XenosBeamBlit.plugin "$scratch/fix/XenonFramebufferTest.kext/Contents/Info.plist"
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
' < boot-stage/XWBM.MKE
