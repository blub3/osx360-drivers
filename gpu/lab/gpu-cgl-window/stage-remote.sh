set -eu
export LC_ALL=C
boot=/Volumes/OSX360BOOT
target=XenosCGLWindowTest
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
verify 2a15e64f7e185e0790d5780b578fb016 kboot.conf
verify 2da466ec6ff69691f4c8cfa3ce29cf3f kboot.conf.before-gpucgloffscreen.bak
verify 8ac34c28a1cc94f3ccd7bf9be6eeaa64 XenosCGL.mkext
verify b99d4be9377e3e7965bb3e88d5f1ff0e NativeHD.mkext
verify ca72c036347285e3fe77b58d3b2c93c6 openbios-xbox360-native-hdmi-test.elf
verify 2ce8cf67dd57d0ef8b68beaa342e051a openbios-xbox360.720p-hdmi.elf.bak

scratch=$(mktemp -d /tmp/xenos-cgl-window-stage.XXXXXXXX)
incoming=$(mktemp -d "$boot/.xenos-cgl-window-incoming.XXXXXXXX")
trap 'rm -rf "$scratch" "$incoming"' 0
trap 'exit 1' HUP INT TERM
boot_checksums() {
    find . \( -path "./$target" -o -path "./${incoming##*/}" \) -prune -o -type f -print0 |
        xargs -0 md5 | sort
}
verify_payload() {
    verify "$expected_app" "$1/XenosCGLWindow"
    verify "$expected_plugin" "$1/GLEngineXenosWindow"
    verify "$expected_runner" "$1/RunXenosCGLWindow.sh"
    test -x "$1/XenosCGLWindow"
}
boot_checksums > "$scratch/before"
tar -xmf - -C "$incoming"
verify_payload "$incoming/$target"
mv -n "$incoming/$target" "$boot/$target"
test ! -e "$incoming/$target"
sync
verify_payload "$boot/$target"
boot_checksums > "$scratch/after"
cmp "$scratch/before" "$scratch/after"
printf 'Verified %s pre-existing files and staged %s.\n' \
    "$(wc -l < "$scratch/after" | tr -d ' ')" "$boot/$target"
