set -eu
export PATH=/usr/bin:/bin:/usr/sbin:/sbin LC_ALL=C
boot=/Volumes/OSX360BOOT
/sbin/mount | grep -Fq " on $boot ("
case ${1:-single} in
    overlap)
        name=XenosWinOverlap testdir=XenosWindowOverlapTest
        info_digest=fcab1b2748d13f9cd51b53f7f5aaec11 exe_digest=0560680747e7f4c87c15afe6dca0e9ce ;;
    single)
        name=XenosWinAuto testdir=XenosWindowUnarmedTest
        info_digest=c4419914b4e58a6339909b964230b7dd exe_digest=3ef163929985ee3f3aa631ce5b659758 ;;
    *) exit 2 ;;
esac
bundle=$name.plugin
source=$boot/$testdir/$bundle/Contents
extensions=/System/Library/Extensions
dest=$extensions/$bundle

verify() {
    test "$(md5 -q "$1/Info.plist")" = "$info_digest" &&
    test "$(md5 -q "$1/MacOS/$name")" = "$exe_digest"
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
echo "$name plug-in install complete."
