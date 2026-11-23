set -eu
export LC_ALL=C
boot=${BOOT_VOLUME:-/Volumes/OSX360BOOT}
case "$boot" in /Volumes/OSX360BOOT) ;; *) exit 1 ;; esac
/sbin/mount | grep -Fq " on $boot ("
cd "$boot"
test "$(pwd -P)" = "$boot"
grep -Fx 'default=gpuwintile' kboot.conf
test -f XWTC.MKE && test -f WINTILE.ELF
test -f NativeHD.mkext && test -f openbios-xbox360.720p-hdmi.elf.bak
test ! -e XenosWindowTileTest/repaint-window.arm
test ! -e XenosWindowTileTest/repaint-window.lock

archive=XWAU.MKE
firmware=WINAUTO.ELF
config=kboot-gpu-window-unarmed.conf
backup=kboot.conf.before-gpuwinauto-stage.bak
testdir=XenosWindowUnarmedTest
names="$archive $firmware $config $backup $testdir"
verify_payload() {
    printf '%s\n' "$manifest" | while read -r digest name; do
        test "$(md5 -q "$1/$name")" = "$digest"
    done
}

if [ -f "$config" ]; then
    cat >/dev/null
    verify_payload "$boot"
    cmp "$backup" kboot.conf
    echo "gpuwinauto is already staged. gpuwintile is still the default."
    exit 0
fi
for name in $names; do
    test ! -e "$name" && test ! -L "$name"
done

scratch=$(mktemp -d /tmp/xenos-winauto.XXXXXXXX)
incoming=$(mktemp -d "$boot/.xenos-winauto-incoming.XXXXXXXX")
published=
cleanup() {
    status=$?
    trap - 0
    if [ "$status" -ne 0 ]; then
        for name in $published; do
            rm -rf "$boot/$name"
        done
    fi
    rm -rf "$scratch" "$incoming"
    exit "$status"
}
trap cleanup 0
trap 'exit 1' HUP INT TERM

boot_checksums() {
    find . \( -path "./$archive" -o -path "./$firmware" -o -path "./$config" \
        -o -path "./$backup" -o -path "./$testdir" -o -path "./${incoming##*/}" \) \
        -prune -o -type f -print0 | xargs -0 md5 -r | sort
}

boot_checksums > "$scratch/before"
tar -xmf - -C "$incoming"
verify_payload "$incoming"
cmp kboot.conf "$incoming/$backup"
grep -Fx 'default=gpuwinauto' "$incoming/$config"
grep -Fx 'gpuwintile="uda0:/WINTILE.ELF"' "$incoming/$config"
for name in $names; do
    mv -n "$incoming/$name" "$boot/$name"
    test ! -e "$incoming/$name"
    published="$published $name"
done
sync
verify_payload "$boot"
boot_checksums > "$scratch/after"
cmp "$scratch/before" "$scratch/after"
printf 'Verified %s pre-existing files. gpuwinauto staged, gpuwintile remains the default.\n' \
    "$(wc -l < "$scratch/after" | tr -d ' ')"
