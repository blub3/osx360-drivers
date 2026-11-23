set -eu
export LC_ALL=C
boot=/Volumes/OSX360BOOT
target=XenosCGLDiscoveryTest
cd "$boot"
test "$(pwd -P)" = "$boot"
test ! -e "$target"
verify() {
    actual=$(md5 -q "$2")
    if test "$actual" != "$1"; then
        echo "Checksum mismatch for $2: $actual" >&2
        exit 1
    fi
}
verify 2da466ec6ff69691f4c8cfa3ce29cf3f kboot.conf
verify 8c2c7324d86de82840dede3d238135e8 kboot.conf.before-gpumulticlient.bak
verify ca72c036347285e3fe77b58d3b2c93c6 openbios-xbox360-native-hdmi-test.elf
verify 2ce8cf67dd57d0ef8b68beaa342e051a openbios-xbox360.720p-hdmi.elf.bak

scratch=$(mktemp -d /tmp/xenos-cgl-stage.XXXXXXXX)
incoming=$(mktemp -d "$boot/.xenos-cgl-incoming.XXXXXXXX")
trap 'rm -rf "$scratch" "$incoming"' 0
trap 'exit 1' HUP INT TERM
boot_checksums() {
    find . \( -path "./$target" -o -path "./${incoming##*/}" \) -prune -o -type f -print0 |
        xargs -0 md5 | sort
}
boot_checksums > "$scratch/before"
tar -xmf - -C "$incoming"
verify "$expected_app" "$incoming/$target/XenosCGLDiscovery"
verify "$expected_bundle" "$incoming/$target/GLDriverXenos"
verify "$expected_runner" "$incoming/$target/RunXenosCGLDiscovery.sh"
test -x "$incoming/$target/XenosCGLDiscovery"
mv -n "$incoming/$target" "$boot/$target"
test ! -e "$incoming/$target"
sync
verify "$expected_app" "$target/XenosCGLDiscovery"
verify "$expected_bundle" "$target/GLDriverXenos"
verify "$expected_runner" "$target/RunXenosCGLDiscovery.sh"
boot_checksums > "$scratch/after"
cmp "$scratch/before" "$scratch/after"
printf 'Verified %s existing boot-volume files and staged %s.\n' \
    "$(wc -l < "$scratch/after" | tr -d ' ')" "$boot/$target"
