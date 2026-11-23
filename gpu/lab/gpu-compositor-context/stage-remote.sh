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
verify 1c9b0443b89ba18117700c59b2e6e577 kboot.conf
verify 6d5c258b4b5adb7af7f78c35c932bff9 kboot.conf.before-gpuwsfbretry.bak
verify 9d6c4d2bc106bc335b6e4f840b0a7083 XenosWFR.mkext
verify 39f90e52965b079222712f235384c869 openbios-xbox360-gpu-windowserver-framebuffer.elf
verify 5790ac8b037422bf101f57fd9e51c55b kboot.conf.before-gpuwssubmit.bak
verify 18663e43a5af0009a381b562cb5ca66d XenosWSP.mkext
verify 746b948434ee1eb0f6a56876cb452f8c openbios-xbox360-gpu-windowserver-submit.elf
verify 653ae06ff70d3e16e62f9f0c6cbfc478 /System/Library/Extensions/GLDriverXenosNative.bundle/Contents/MacOS/GLDriverXenosNative
verify 228691583a418df3e68ce58b5c8adca9 kboot.conf.before-gpudisplaysurface.bak
verify c717c0c0606b19b4eb13b48a403e107f XenosWSS.mkext
verify 6e6f5ec3ac7e547f4146f61b2cd05406 openbios-xbox360-gpu-display-surface.elf
verify 6f70043baec8ea1b5280136cf16856be kboot.conf.before-gpunativegld.bak
verify cfc64a836626cfdef2793a0ad17f8da3 XenosGLD.mkext
verify d02059c9886d38f103e85f382bab3b72 openbios-xbox360-gpu-native-gld.elf
verify 2a15e64f7e185e0790d5780b578fb016 kboot.conf.before-gpuxenoswsretry.bak
verify 4ef8f13100a98b993d9896cd709f4e38 XenosQER.mkext
verify 8ac34c28a1cc94f3ccd7bf9be6eeaa64 XenosCGL.mkext
verify b99d4be9377e3e7965bb3e88d5f1ff0e NativeHD.mkext
verify ca72c036347285e3fe77b58d3b2c93c6 openbios-xbox360-native-hdmi-test.elf
verify 2ce8cf67dd57d0ef8b68beaa342e051a openbios-xbox360.720p-hdmi.elf.bak
echo "Verified active profile, rollback, native HDMI, and 720p backup before staging."
archive=XenosWCC.mkext
firmware=openbios-xbox360-gpu-compositor-context.elf
config=kboot-gpu-compositor-context.conf
testdir=XenosCompositorContextTest
for name in "$archive" "$firmware" "$config" "$testdir"; do
    test ! -e "$name"
done
scratch=$(mktemp -d /tmp/xenos-ws-framebuffer-stage.XXXXXXXX)
incoming=$(mktemp -d "$boot/.xenos-compositor-incoming.XXXXXXXX")
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
printf 'Verified %s pre-existing files and staged gpucompositor.\n' \
    "$(wc -l < "$scratch/after" | tr -d ' ')"
grep '^default=' kboot.conf "$config"
