set -eu
export PATH=/usr/bin:/bin:/usr/sbin:/sbin
export LC_ALL=C
/sbin/mount | grep -Fq " on /Volumes/OSX360BOOT ("
bundle=XenosWinTile.plugin
source=/Volumes/OSX360BOOT/XenosWindowTileTest/$bundle/Contents
extensions=/System/Library/Extensions
dest=$extensions/$bundle
exe=XenosWinTile

verify() {
    test "$(md5 -q "$1/Info.plist")" = 88ab65cc6244e13f3d747fc2f65df7af
    test "$(md5 -q "$1/MacOS/$exe")" = f2c1f3f24223c41bf14b1634709d4d7f
}
verify "$source"
if test -e "$dest" || test -L "$dest"; then
    if test -L "$dest" || ! verify "$dest/Contents"; then
        echo "A different blit plug-in already exists at $dest" >&2
        exit 1
    fi
else
    umask 022
    temporary=$(mktemp -d "$extensions/.xenos-window-tile-plugin.XXXXXXXX")
    trap "rm -rf \"$temporary\"" 0
    trap "exit 1" HUP INT TERM
    mkdir -p "$temporary/$bundle/Contents/MacOS"
    contents=$temporary/$bundle/Contents
    /usr/bin/install -m 644 "$source/Info.plist" "$contents/Info.plist"
    /usr/bin/install -m 755 "$source/MacOS/$exe" "$contents/MacOS/$exe"
    /usr/sbin/chown -R root:wheel "$temporary/$bundle"
    verify "$contents"
    /usr/bin/plutil -lint "$contents/Info.plist"
    test ! -e "$dest" && test ! -L "$dest"
    mv -n "$temporary/$bundle" "$dest"
    test ! -e "$temporary/$bundle"
    sync
fi
verify "$dest/Contents"
for file in "$dest" "$dest/Contents" "$dest/Contents/MacOS" "$dest/Contents/Info.plist" "$dest/Contents/MacOS/$exe"; do
    test ! -L "$file"
    test "$(stat -f "%Su:%Sg" "$file")" = root:wheel
done
ls -ld "$dest" "$dest/Contents/Info.plist" "$dest/Contents/MacOS/$exe"
echo "XenosWinTile plug-in install complete."
