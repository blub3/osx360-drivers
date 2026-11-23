set -eu
export LC_ALL=C
boot=${BOOT_VOLUME:-/Volumes/OSX360BOOT}
case "$boot" in /Volumes/OSX360BOOT) ;; *) exit 1 ;; esac
/sbin/mount | grep -Fq " on $boot ("
cd "$boot"
test "$(pwd -P)" = "$boot"
case "$variant" in
    overlap)
        parent=gpuwinauto parent_elf=WINAUTO.ELF parent_archive=XWAU.MKE
        profile=gpuwinoverlap archive=XWOV.MKE firmware=WINOVR.ELF
        config=kboot-gpu-window-unarmed-overlap.conf testdir=XenosWindowOverlapTest ;;
    single)
        parent=gpuwintile parent_elf=WINTILE.ELF parent_archive=XWTC.MKE
        profile=gpuwinauto archive=XWAU.MKE firmware=WINAUTO.ELF
        config=kboot-gpu-window-unarmed.conf testdir=XenosWindowUnarmedTest ;;
    *) exit 2 ;;
esac
grep -Fx "default=$parent" kboot.conf
test -f "$parent_archive"
test -f "$parent_elf"
test -f NativeHD.mkext
test -f openbios-xbox360-native-hdmi-test.elf
test -f openbios-xbox360.720p-hdmi.elf.bak
test ! -e XenosWindowTileTest/repaint-window.arm
test ! -e XenosWindowTileTest/repaint-window.lock

backup=kboot.conf.before-$profile-stage.bak
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
    echo "$profile is already staged. $parent is still the default."
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
grep -Fx "default=$profile" "$incoming/$config"
grep -Fx "$parent=\"uda0:/$parent_elf\"" "$incoming/$config"
for name in $names; do
    mv -n "$incoming/$name" "$boot/$name"
    test ! -e "$incoming/$name"
    published="$published $name"
done
sync
verify_payload "$boot"
boot_checksums > "$scratch/after"
cmp "$scratch/before" "$scratch/after"
printf 'Verified %s pre-existing files. %s staged, %s remains the default.\n' \
    "$(wc -l < "$scratch/after" | tr -d ' ')" "$profile" "$parent"
