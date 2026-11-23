set -eu
export PATH=/usr/bin:/bin:/usr/sbin:/sbin LC_ALL=C
boot=/Volumes/OSX360BOOT
/sbin/mount | grep -Fq " on $boot ("
name=XenosWinAuto
bundle=$name.plugin
source=$boot/XenosWindowUnarmedTest/$bundle/Contents
extensions=/System/Library/Extensions
dest=$extensions/$bundle

verify() {
    test "$(md5 -q "$1/Info.plist")" = c4419914b4e58a6339909b964230b7dd &&
    test "$(md5 -q "$1/MacOS/$name")" = 3ef163929985ee3f3aa631ce5b659758
}
verify "$source"
if [ ! -e "$dest" ] && [ ! -L "$dest" ]; then
    umask 022
    scratch=$(mktemp -d "$extensions/.xenos-winauto.XXXXXXXX")
    cleanup() { rm -rf "$scratch"; }
    trap cleanup 0
    stage=$scratch/$bundle/Contents
    mkdir -p "$stage/MacOS"
    /usr/bin/install -o root -g wheel -m 644 "$source/Info.plist" "$stage/Info.plist"
    /usr/bin/install -o root -g wheel -m 755 "$source/MacOS/$name" "$stage/MacOS/$name"
    verify "$stage"
    /usr/bin/plutil -lint "$stage/Info.plist"
    test ! -e "$dest" && test ! -L "$dest"
    mv -n "$scratch/$bundle" "$dest"
    test ! -e "$scratch/$bundle"
    sync
fi
for item in "$dest" "$dest/Contents" "$dest/Contents/MacOS" "$dest/Contents/Info.plist" "$dest/Contents/MacOS/$name"; do
    test ! -L "$item"
    test "$(stat -f "%Su:%Sg" "$item")" = root:wheel
done
verify "$dest/Contents"
test "$(stat -f "%Lp" "$dest/Contents/Info.plist")" = 644
test "$(stat -f "%Lp" "$dest/Contents/MacOS/$name")" = 755
echo "XenosWinAuto plug-in install complete."
