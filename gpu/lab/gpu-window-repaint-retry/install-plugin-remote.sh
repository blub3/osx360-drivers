set -eu
export PATH=/usr/bin:/bin:/usr/sbin:/sbin
export LC_ALL=C
/sbin/mount | grep -Fq " on /Volumes/OSX360BOOT ("
bundle=XenosWinFix.plugin
source=/Volumes/OSX360BOOT/XenosWindowRetryTest/$bundle/Contents
extensions=/System/Library/Extensions
dest=$extensions/$bundle
exe=XenosWinFix

verify() {
    test "$(md5 -q "$1/Info.plist")" = 7085f52896edcff439f0a27a89042a2f
    test "$(md5 -q "$1/MacOS/$exe")" = 71f7678814b8fd65017ad2e320b1029f
}
verify "$source"
if test -e "$dest" || test -L "$dest"; then
    if test -L "$dest" || ! verify "$dest/Contents"; then
        echo "A different blit plug-in already exists at $dest" >&2
        exit 1
    fi
else
    umask 022
    temporary=$(mktemp -d "$extensions/.xenos-window-retry-plugin.XXXXXXXX")
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
echo "XenosWinFix plug-in install complete."
