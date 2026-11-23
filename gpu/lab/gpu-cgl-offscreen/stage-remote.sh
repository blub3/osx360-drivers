set -eu
export LC_ALL=C
boot=/Volumes/OSX360BOOT
cd "$boot"
test "$(pwd -P)" = "$boot"
archive=XenosCGL.mkext
firmware=openbios-xbox360-gpu-cgl-offscreen.elf
config=kboot-gpu-cgl-offscreen.conf
app=XenosCGLOffscreenTest

verify() {
    actual=$(md5 -q "$2")
    if test "$actual" != "$1"; then
        echo "Checksum mismatch for $2: $actual" >&2
        exit 1
    fi
}
verify 2da466ec6ff69691f4c8cfa3ce29cf3f kboot.conf
verify 8c2c7324d86de82840dede3d238135e8 kboot.conf.before-gpumulticlient.bak
verify 6e1ccd6bc9872628ac7dd00d70238720 XenosMCL.mkext
verify ca72c036347285e3fe77b58d3b2c93c6 openbios-xbox360-native-hdmi-test.elf
verify 2ce8cf67dd57d0ef8b68beaa342e051a openbios-xbox360.720p-hdmi.elf.bak
for name in "$archive" "$firmware" "$config" "$app"; do
    test ! -e "$name"
done
scratch=$(mktemp -d /tmp/xenos-cgl-stage.XXXXXXXX)
incoming=$(mktemp -d "$boot/.xenos-cgl-incoming.XXXXXXXX")
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
    test -x "$1/$app/XenosCGLOffscreen"
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
printf 'Verified %s pre-existing files and staged gpucgloffscreen.\n' \
    "$(wc -l < "$scratch/after" | tr -d ' ')"
grep '^default=' kboot.conf "$config"
