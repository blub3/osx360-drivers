set -eu
export PATH=/usr/bin:/bin:/usr/sbin:/sbin
export LC_ALL=C
/sbin/mount | grep -Fq " on /Volumes/OSX360BOOT ("
bundle=XenosLegacyBlit.plugin
source=/Volumes/OSX360BOOT/XenosFramebufferBlitTest/$bundle/Contents
extensions=/System/Library/Extensions
dest=$extensions/$bundle
exe=XenosLegacyBlit

verify() {
    test "$(md5 -q "$1/Info.plist")" = 14cf4a695008d3ff43a00b3ed1303545
    test "$(md5 -q "$1/MacOS/$exe")" = f3f4a7603891bd58198174f35741ab05
}
verify "$source"
if test -e "$dest" || test -L "$dest"; then
    if test -L "$dest" || ! verify "$dest/Contents"; then
        echo "A different blit plug-in already exists at $dest" >&2
        exit 1
    fi
else
    umask 022
    temporary=$(mktemp -d "$extensions/.xenos-fb-blit-plugin.XXXXXXXX")
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
echo "Xenos blit plug-in install complete."
