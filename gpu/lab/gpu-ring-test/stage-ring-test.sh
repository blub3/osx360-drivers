set -eu
boot=/Volumes/OSX360BOOT
cd "$boot"
test "$(pwd -P)" = "$boot"

verify() {
    actual=$(md5 -q "$2")
    if test "$actual" != "$1"; then
        echo "Checksum mismatch for $2: $actual" >&2
        exit 1
    fi
}

boot_checksums() {
    md5 *.bak kboot.conf kboot-native-hdmi-test.conf kboot-framebuffer-test.conf \
        openbios-xbox360.elf Xbox360.mkext XenosFB.mkext NativeHD.mkext \
        openbios-xbox360-framebuffer-test.elf openbios-xbox360-native-hdmi-test.elf
}

archive=XenosGPU.mkext
elf=openbios-xbox360-gpu-ring-test.elf
config=kboot-gpu-ring-test.conf
verify 7ee412f046fff91d1fec7713ebbf749b kboot.conf
verify ca72c036347285e3fe77b58d3b2c93c6 openbios-xbox360-native-hdmi-test.elf
verify b99d4be9377e3e7965bb3e88d5f1ff0e NativeHD.mkext
verify 2ce8cf67dd57d0ef8b68beaa342e051a openbios-xbox360.720p-hdmi.elf.bak
for name in "$archive" "$elf" "$config"; do
    if test -e "$name"; then
        echo "$name already exists" >&2
        exit 1
    fi
done

scratch=$(mktemp -d /tmp/xenos-ring-stage.XXXXXXXX)
trap 'rm -rf "$scratch"' EXIT
boot_checksums > "$scratch/before"
/usr/bin/tar -xf - -C "$scratch"
verify "$archive_md5" "$scratch/$archive"
verify "$elf_md5" "$scratch/$elf"
verify "$config_md5" "$scratch/$config"
mkdir "$scratch/unpacked"
/usr/sbin/mkextunpack -v -a ppc -d "$scratch/unpacked" "$scratch/$archive" > "$scratch/bundles"
cat "$scratch/bundles"
test "$(find "$scratch/unpacked" -name '*.kext' -type d | wc -l | tr -d ' ')" = 9
grep -F 'XenonCoreGraphics - fish.goldfish64.osx360.XenonCoreGraphics (0.0.5d1)' "$scratch/bundles" >/dev/null
/sbin/kextload -n -t -z "$scratch/unpacked/XenonCoreGraphics.kext"

cp -n "$scratch/$archive" "$scratch/$elf" "$scratch/$config" "$boot/"
sync
verify "$archive_md5" "$archive"
verify "$elf_md5" "$elf"
verify "$config_md5" "$config"
boot_checksums > "$scratch/after"
cmp "$scratch/before" "$scratch/after"
printf 'Verified the staged ring test and %s preserved boot files and backups.\n' "$(wc -l < "$scratch/after" | tr -d ' ')"
