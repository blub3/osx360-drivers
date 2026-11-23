set -eu
export LC_ALL=C
boot=/Volumes/OSX360BOOT
cd "$boot"
test "$(pwd -P)" = "$boot"
archive=XenosQES.mkext
firmware=openbios-xbox360-gpu-windowserver.elf
config=kboot-gpu-windowserver.conf
app=XenosWindowServerTest

verify() {
    actual=$(md5 -q "$2")
    if test "$actual" != "$1"; then
        echo "Checksum mismatch for $2: $actual" >&2
        exit 1
    fi
}
verify 2a15e64f7e185e0790d5780b578fb016 kboot.conf
verify 8c2c7324d86de82840dede3d238135e8 kboot.conf.before-gpumulticlient.bak
verify 6e1ccd6bc9872628ac7dd00d70238720 XenosMCL.mkext
verify 8ac34c28a1cc94f3ccd7bf9be6eeaa64 XenosCGL.mkext
verify ca72c036347285e3fe77b58d3b2c93c6 openbios-xbox360-native-hdmi-test.elf
verify 2ce8cf67dd57d0ef8b68beaa342e051a openbios-xbox360.720p-hdmi.elf.bak
for name in "$archive" "$firmware" "$config" "$app"; do
    test ! -e "$name"
done
scratch=$(mktemp -d /tmp/xenos-qe-stage.XXXXXXXX)
incoming=$(mktemp -d "$boot/.xenos-qe-incoming.XXXXXXXX")
trap 'rm -rf "$scratch" "$incoming"' 0
trap 'exit 1' HUP INT TERM
boot_checksums() {
    find . \( -path "./$app" -o -path "./$archive" -o -path "./$firmware" \
             -o -path "./$config" -o -path "./${incoming##*/}" \) -prune -o -type f -print0 |
        xargs -0 md5 | sort
}
verify_payload() {
    printf '%s\n' "$manifest" | while read -r digest filename; do
        verify "$digest" "$1/$filename"
    done
    test -x "$1/$app/XenosWindowServerCheck"
}
boot_checksums > "$scratch/before"
tar -xmf - -C "$incoming"
verify_payload "$incoming"
for name in "$archive" "$firmware" "$app" "$config"; do
    mv -n "$incoming/$name" "$boot/$name"
    test ! -e "$incoming/$name"
done
sync
verify_payload "$boot"
boot_checksums > "$scratch/after"
cmp "$scratch/before" "$scratch/after"
printf 'Verified %s pre-existing files and staged gpuxenosws.\n' \
    "$(wc -l < "$scratch/after" | tr -d ' ')"
grep '^default=' kboot.conf "$config"
