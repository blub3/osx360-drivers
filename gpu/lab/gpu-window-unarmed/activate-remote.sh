set -eu
export LC_ALL=C
boot=/Volumes/OSX360BOOT
/sbin/mount | grep -Fq " on $boot ("
cd "$boot"
test "$(pwd -P)" = "$boot"

old=393ef935a095cc9aaf570b9ce3a8d511
new=971cd5a4d6a6ae40bc1878ca6092d2d6
candidate=kboot-gpu-window-unarmed.conf
backup=kboot.conf.before-gpuwinauto.bak
verify() {
    if [ "$(md5 -q "$2")" != "$1" ]; then
        printf 'Checksum mismatch for %s\n' "$2" >&2
        exit 1
    fi
}

verify "$old" kboot.conf.before-gpuwinauto-stage.bak
verify "$new" "$candidate"
verify 40dda24eabaaf7b89821a81f5ac2f2ea XWAU.MKE
verify 83bf102f2f8ae255b384745cbcaae554 WINAUTO.ELF
verify c36e84a7e31782ea14a20b3a931881fb XWTC.MKE
verify 42a56267336d32c99565f24d9c9bf8f2 WINTILE.ELF
verify b99d4be9377e3e7965bb3e88d5f1ff0e NativeHD.mkext
verify ca72c036347285e3fe77b58d3b2c93c6 openbios-xbox360-native-hdmi-test.elf
verify 2ce8cf67dd57d0ef8b68beaa342e051a openbios-xbox360.720p-hdmi.elf.bak
for name in XenosWinAuto XenosWinTile; do
    contents=/System/Library/Extensions/$name.plugin/Contents
    test "$(stat -f '%Su:%Sg:%Lp' "$contents/Info.plist")" = root:wheel:644
    test "$(stat -f '%Su:%Sg:%Lp' "$contents/MacOS/$name")" = root:wheel:755
done
verify c4419914b4e58a6339909b964230b7dd /System/Library/Extensions/XenosWinAuto.plugin/Contents/Info.plist
verify 3ef163929985ee3f3aa631ce5b659758 /System/Library/Extensions/XenosWinAuto.plugin/Contents/MacOS/XenosWinAuto
verify 88ab65cc6244e13f3d747fc2f65df7af /System/Library/Extensions/XenosWinTile.plugin/Contents/Info.plist
verify f2c1f3f24223c41bf14b1634709d4d7f /System/Library/Extensions/XenosWinTile.plugin/Contents/MacOS/XenosWinTile
test ! -e XenosWindowTileTest/repaint-window.arm
test ! -e XenosWindowTileTest/repaint-window.lock

current=$(md5 -q kboot.conf)
if [ "$current" = "$new" ]; then
    verify "$old" "$backup"
    echo "gpuwinauto is already selected."
    exit 0
fi
verify "$old" kboot.conf
grep -Fx 'default=gpuwinauto' "$candidate"
grep -Fx 'gpuwinauto="uda0:/WINAUTO.ELF"' "$candidate"
grep -Fx 'gpuwintile="uda0:/WINTILE.ELF"' "$candidate"

scratch=$(mktemp -d /tmp/xenos-winauto-activate.XXXXXXXX)
temp=
replacing=no
cleanup() {
    status=$?
    trap - 0
    if [ "$status" -ne 0 ] && [ "$replacing" = yes ] &&
       [ "$(md5 -q kboot.conf)" = "$new" ] && [ "$(md5 -q "$backup")" = "$old" ]; then
        restore=$(mktemp "$boot/.kboot-gpuwinauto.XXXXXXXX") &&
            cat "$backup" > "$restore" && mv -f "$restore" kboot.conf && sync &&
            printf 'Restored the previous gpuwintile boot menu.\n' >&2
    fi
    test -z "$temp" || rm -f "$temp"
    rm -rf "$scratch"
    exit "$status"
}
trap cleanup 0
trap 'exit 1' HUP INT TERM

protected_checksums() {
    find . \( -path './kboot.conf' -o -path "./$backup" -o -path './.kboot-gpuwinauto.*' \) \
        -prune -o -type f -print0 | xargs -0 md5 | sort
}

protected_checksums > "$scratch/before"
if [ ! -e "$backup" ] && [ ! -L "$backup" ]; then
    (set -C; cat kboot.conf > "$backup")
fi
verify "$old" "$backup"
verify "$old" kboot.conf
temp=$(mktemp "$boot/.kboot-gpuwinauto.XXXXXXXX")
cat "$candidate" > "$temp"
verify "$new" "$temp"
previous_boot=$(/usr/sbin/sysctl -n kern.boottime)
case "$previous_boot" in ''|*[!0-9]*) exit 1 ;; esac
sync
replacing=yes
mv -f "$temp" kboot.conf
sync
verify "$new" kboot.conf
verify "$old" "$backup"
protected_checksums > "$scratch/after"
cmp "$scratch/before" "$scratch/after"
printf 'Verified %s protected files. gpuwinauto is selected.\n' \
    "$(wc -l < "$scratch/after" | tr -d ' ')"
printf 'previous_boot=%s\n' "$previous_boot"
