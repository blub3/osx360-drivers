set -eu
bundle=GLDriverXenosWSProbe.bundle
source=/Volumes/OSX360BOOT/XenosWindowServerSubmitTest/$bundle/Contents
extensions=/System/Library/Extensions
dest=$extensions/$bundle
exe=GLDriverXenosWSProbe

verify() {
    test "$(md5 -q "$1/Info.plist")" = 0360a00c8eb911aeb51258f3077c5185
    test "$(md5 -q "$1/MacOS/$exe")" = d3ec0ee7b5cc2013e0c256c0d9c742f1
}
verify "$source"
if test -e "$dest" || test -L "$dest"; then
    if test -L "$dest" || ! verify "$dest/Contents"; then
        echo "A different GLD bundle already exists at $dest" >&2
        exit 1
    fi
else
    umask 022
    temporary=$(mktemp -d "$extensions/.xenos-windowserver-gld.XXXXXXXX")
    trap "rm -rf \"$temporary\"" 0
    trap "exit 1" HUP INT TERM
    mkdir -p "$temporary/$bundle/Contents/MacOS"
    contents=$temporary/$bundle/Contents
    /usr/bin/install -m 644 "$source/Info.plist" "$contents/Info.plist"
    /usr/bin/install -m 755 "$source/MacOS/$exe" "$contents/MacOS/$exe"
    /usr/sbin/chown -R root:wheel "$temporary/$bundle"
    verify "$contents"
    test ! -e "$dest" && test ! -L "$dest"
    mv -n "$temporary/$bundle" "$dest"
    test ! -e "$temporary/$bundle"
    sync
fi
verify "$dest/Contents"
ls -ld "$dest" "$dest/Contents/Info.plist" "$dest/Contents/MacOS/$exe"
echo "WindowServer GLD bundle install complete."
