# Read a tar of the candidate archive and test bundle on the running gpuwintile profile.
set -eu
export LC_ALL=C
boot=/Volumes/OSX360BOOT
scratch=$(mktemp -d /tmp/xenos-winauto-preflight.XXXXXXXX)
trap 'rm -rf "$scratch"' 0
trap 'exit 1' HUP INT TERM
grep -Fx 'default=gpuwintile' "$boot/kboot.conf"
test ! -e "$boot/XenosWindowTileTest/repaint-window.arm"
test ! -e "$boot/XenosWindowTileTest/repaint-window.lock"
cd "$scratch"
tar -xmf -
mkdir before candidate
/usr/sbin/mkextunpack -v -a ppc -d before "$boot/XWTC.MKE" > before.list
/usr/sbin/mkextunpack -v -a ppc -d candidate XWAU.MKE > candidate.list
cmp before.list candidate.list
count=0
for kext in before/*.kext; do
    name=${kext##*/}
    cmp "$kext/Contents/MacOS/${name%.kext}" "candidate/$name/Contents/MacOS/${name%.kext}"
    new="candidate/$name/Contents/Info.plist"
    if [ "$name" = XenonFramebufferTest.kext ]; then
        /usr/bin/plutil -lint "$new"
        sed 's/XenosWinTile\.plugin/XenosWinAuto.plugin/g' "$kext/Contents/Info.plist" > expected.plist
        cmp expected.plist "$new"
    else
        cmp "$kext/Contents/Info.plist" "$new"
    fi
    count=$((count + 1))
done
test "$count" -eq 9
printf 'Tiger verified %s identical kext executables and only the framebuffer plug-in name changed.\n' "$count"

testdir=XenosWindowUnarmedTest
/usr/bin/plutil -lint "$testdir/XenosWinAuto.plugin/Contents/Info.plist"
/bin/sh "$testdir/RunXenosWindowUnarmed.sh" --plugin-control
if ! /bin/sh "$testdir/RunXenosWindowUnarmed.sh" --baseline; then
    echo "The software window-move control failed on the running boot." >&2
    exit 1
fi
test ! -e "$boot/XenosWindowTileTest/repaint-window.arm"
test ! -e "$boot/XenosWindowTileTest/repaint-window.lock"
