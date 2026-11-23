set -eu
bundle=GLDriverXenosNative.bundle
source=/Volumes/OSX360BOOT/XenosNativeGLDTest/$bundle/Contents
extensions=/System/Library/Extensions
dest=$extensions/$bundle
exe=GLDriverXenosNative

verify() {
    test "$(md5 -q "$1/Info.plist")" = b67cb572421876ce39a9213d7800ae8f
    test "$(md5 -q "$1/MacOS/$exe")" = 653ae06ff70d3e16e62f9f0c6cbfc478
}
verify "$source"
if test -e "$dest" || test -L "$dest"; then
    if test -L "$dest" || ! verify "$dest/Contents"; then
        echo "A different GLD bundle already exists at $dest" >&2
        exit 1
    fi
else
    umask 022
    temporary=$(mktemp -d "$extensions/.xenos-native-gld.XXXXXXXX")
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
echo "Xenos native GLD bundle install complete."
