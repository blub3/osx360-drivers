set -eu
export LC_ALL=C
boot=/Volumes/OSX360BOOT
cd "$boot"
test "$(pwd -P)" = "$boot"
verify() {
    actual=$(md5 -q "$2")
    if [ "$actual" != "$1" ]; then
        echo "Checksum mismatch for $2: $actual" >&2
        exit 1
    fi
}
verify 6f70043baec8ea1b5280136cf16856be kboot.conf
verify 2a15e64f7e185e0790d5780b578fb016 kboot.conf.before-gpuxenoswsretry.bak
verify 4ef8f13100a98b993d9896cd709f4e38 XenosQER.mkext
verify 8ac34c28a1cc94f3ccd7bf9be6eeaa64 XenosCGL.mkext
verify b99d4be9377e3e7965bb3e88d5f1ff0e NativeHD.mkext
verify ca72c036347285e3fe77b58d3b2c93c6 openbios-xbox360-native-hdmi-test.elf
verify 2ce8cf67dd57d0ef8b68beaa342e051a openbios-xbox360.720p-hdmi.elf.bak
echo "Verified active profile, rollback, native HDMI, and 720p backup before staging."
archive=XenosGLD.mkext
firmware=openbios-xbox360-gpu-native-gld.elf
config=kboot-gpu-native-gld.conf
testdir=XenosNativeGLDTest
for name in "$archive" "$firmware" "$config" "$testdir"; do
    test ! -e "$name"
done
scratch=$(mktemp -d /tmp/xenos-native-gld-stage.XXXXXXXX)
incoming=$(mktemp -d "$boot/.xenos-gld-incoming.XXXXXXXX")
trap 'rm -rf "$scratch" "$incoming"' 0
trap 'exit 1' HUP INT TERM
boot_checksums() {
    find . \( -path "./$testdir" -o -path "./$archive" -o -path "./$firmware" \
        -o -path "./$config" -o -path "./${incoming##*/}" \) -prune -o -type f -print0 |
        xargs -0 md5 | sort
}
verify_payload() {
    printf '%s\n' "$manifest" | while read -r digest name; do
        verify "$digest" "$1/$name"
    done
}

boot_checksums > "$scratch/before"
tar -xmf - -C "$incoming"
verify_payload "$incoming"
for name in "$archive" "$firmware" "$config" "$testdir"; do
    mv -n "$incoming/$name" "$boot/$name"
    test ! -e "$incoming/$name"
done
sync
verify_payload "$boot"
boot_checksums > "$scratch/after"
cmp "$scratch/before" "$scratch/after"
printf 'Verified %s pre-existing files and staged gpunativegld.\n' \
    "$(wc -l < "$scratch/after" | tr -d ' ')"
grep '^default=' kboot.conf "$config"
