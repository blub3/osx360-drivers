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
    for path in ./*; do
        case "$path" in
            "./$archive"|"./$elf"|"./$config") continue ;;
        esac
        if test -f "$path"; then
            md5 "$path"
        fi
    done
}

archive=XenosZBF.mkext
elf=openbios-xbox360-gpu-depth-triangles.elf
config=kboot-gpu-depth-triangles.conf
verify 063c5de67ff93a8d50ee286bae52ce01 kboot-gpu-textured-triangle.conf
verify eda27e93b1622bd5277cb5d4a9787054 openbios-xbox360-gpu-textured-triangle.elf
verify 5b83d158a2fde343722cab2c19e2f1eb XenosTEX.mkext
verify f4ab66a9915c7a2126855f6566b5f033 kboot-gpu-color-triangle.conf
verify 0663cc66879c208f9a5d5b07cd526d2f openbios-xbox360-gpu-color-triangle.elf
verify ce2984980b9c5c8486131ee30fe2dfbc XenosRGB.mkext
verify 0dee4bd74c96716933a173f68cccf586 kboot-gpu-triangle-retry.conf
verify 1d002ad58e2d013336ff00810738c695 openbios-xbox360-gpu-triangle-retry.elf
verify d17247c03ac273a63b387dce42017813 XenosTR2.mkext
verify 92074758ee587b1a2f57453e1daf831e kboot-gpu-triangle-test.conf
verify 6e157c770bfc57cb52e42f8873cd1afa openbios-xbox360-gpu-triangle-test.elf
verify b65926c0623ac462da040ebaf0a3d2ec XenosTRI.mkext
verify 3ad3e737996fb6e5b5915e378da4440a kboot-gpu-clear-test.conf
verify 80b9973a7b7e02ad8dcf8177047c3714 openbios-xbox360-gpu-clear-test.elf
verify 7eccce0fc792ac51a030d4b8e6c2c8d3 XenosCLR.mkext
verify 5b7955d3eec06d9f6e1dbaeae47d0b2b kboot-gpu-marker-test.conf
verify f78441e4a22cdef099bf1d7ed3e69bca openbios-xbox360-gpu-marker-test.elf
verify f56201ddaec0cdf991048fef49a65835 XenosCMD.mkext
verify 7addb09477622ba3ad0bdf1339d8d9d0 openbios-xbox360-gpu-ring-test.elf
verify b9e79f4b18ed6a2f5635bab7f96444d7 XenosGPU.mkext
verify 5f7fa41f7e77fe05b567c29f514991ca kboot-gpu-ring-test.conf
verify 7ee412f046fff91d1fec7713ebbf749b kboot-native-hdmi-test.conf
verify ca72c036347285e3fe77b58d3b2c93c6 openbios-xbox360-native-hdmi-test.elf
verify b99d4be9377e3e7965bb3e88d5f1ff0e NativeHD.mkext
verify 2ce8cf67dd57d0ef8b68beaa342e051a openbios-xbox360.720p-hdmi.elf.bak
for name in "$archive" "$elf" "$config"; do
    if test -e "$name"; then
        echo "$name already exists" >&2
        exit 1
    fi
done

scratch=$(mktemp -d /tmp/xenos-depth-triangles-stage.XXXXXXXX)
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
grep -F 'XenonCoreGraphics - fish.goldfish64.osx360.XenonCoreGraphics (0.0.5d8)' "$scratch/bundles" >/dev/null
/sbin/kextload -n -t -z "$scratch/unpacked/XenonCoreGraphics.kext"

cp -n "$scratch/$archive" "$scratch/$elf" "$scratch/$config" "$boot/"
sync
verify "$archive_md5" "$archive"
verify "$elf_md5" "$elf"
verify "$config_md5" "$config"
boot_checksums > "$scratch/after"
cmp "$scratch/before" "$scratch/after"
printf 'Verified the staged depth triangles and %s preserved boot files and backups.\n' "$(wc -l < "$scratch/after" | tr -d ' ')"
