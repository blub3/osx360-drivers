set -eu
boot=/Volumes/OSX360BOOT
cd "$boot"
test "$(pwd -P)" = "$boot"

check_md5() {
    actual=$(md5 -q "$2")
    if test "$actual" != "$1"; then
        echo "Checksum mismatch for $2: $actual" >&2
        exit 1
    fi
}

verify_boot_files() {
    check_md5 36487a07600bfd58dba67d0566d1008d kboot.conf
    check_md5 36487a07600bfd58dba67d0566d1008d kboot-framebuffer-test.conf
    check_md5 937e000a82e6cabc475555f871a85c13 kboot-working.conf.bak
    check_md5 a95a04400a7483e3cffe54b76b8197a7 openbios-xbox360.elf
    check_md5 c9fe3cf65d40779be40b68065e5b14e9 Xbox360.mkext
    check_md5 2ce8cf67dd57d0ef8b68beaa342e051a openbios-xbox360.720p-hdmi.elf.bak
    check_md5 97463fc0ed55968ef041a0369f149cbf openbios-xbox360-framebuffer-test.elf
    check_md5 883676fa89a708021e4a3294c42dda83 XenosFB.mkext
    check_md5 a05186c53fbf0c7d6d3004da6cba33ba XenosFB-v1.mkext.bak
    check_md5 f5046a827720044f3d7db1cb0da53d68 XenosFB-v2.mkext.bak
    check_md5 8336f171bd43f8840a465936e9baec70 XenosFB-v3.mkext.bak
}

verify_new_files() {
    check_md5 "${mkext_md5:?Missing mkext checksum}" "$1/$mkext"
    check_md5 "${elf_md5:?Missing ELF checksum}" "$1/$elf"
    check_md5 "${config_md5:?Missing config checksum}" "$1/$config"
}

mkext=NativeHD.mkext
elf=openbios-xbox360-native-hdmi-test.elf
config=kboot-native-hdmi-test.conf
verify_boot_files
for file in "$mkext" "$elf" "$config"; do
    if test -e "$file"; then
        echo "$file already exists" >&2
        exit 1
    fi
done

scratch=$(mktemp -d /tmp/xenos-native-hdmi.XXXXXXXX)
trap 'rm -rf "$scratch"' EXIT
/usr/bin/tar -xf - -C "$scratch"
verify_new_files "$scratch"
mkdir "$scratch/unpacked"
/usr/sbin/mkextunpack -v -a ppc -d "$scratch/unpacked" "$scratch/$mkext" > "$scratch/bundles"
cat "$scratch/bundles"
test "$(find "$scratch/unpacked" -name '*.kext' -type d | wc -l | tr -d ' ')" = 9
for driver in XenonFramebufferTest XenonSMC; do
    grep -F "$driver - fish.goldfish64.osx360.$driver (0.0.5d1)" "$scratch/bundles" >/dev/null
    /sbin/kextload -n -t -z "$scratch/unpacked/$driver.kext"
done

cp -n "$scratch/$mkext" "$scratch/$elf" "$scratch/$config" "$boot/"
sync
verify_boot_files
verify_new_files "$boot"
echo "Verified separate native HDMI test and preserved boot files on $boot."
