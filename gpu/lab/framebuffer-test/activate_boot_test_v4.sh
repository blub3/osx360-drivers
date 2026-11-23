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
    check_md5 a05186c53fbf0c7d6d3004da6cba33ba XenosFB-v1.mkext.bak
    check_md5 f5046a827720044f3d7db1cb0da53d68 XenosFB-v2.mkext.bak
}

archive=XenosFB.mkext
backup=XenosFB-v3.mkext.bak
old_md5=8336f171bd43f8840a465936e9baec70
: "${new_md5:?Missing v4 archive checksum}"
verify_boot_files
check_md5 "$old_md5" "$archive"

scratch=$(mktemp -d /tmp/xenos-fb-v4.XXXXXXXX)
temp=
trap 'if test -n "$temp"; then rm -f "$temp"; fi; rm -rf "$scratch"' EXIT
temp=$(mktemp "$boot/.xenos-fb-v4.XXXXXXXX")
cat > "$temp"
check_md5 "$new_md5" "$temp"

unpacked=$scratch/unpacked
mkdir "$unpacked"
/usr/sbin/mkextunpack -v -a ppc -d "$unpacked" "$temp" > "$scratch/bundles"
cat "$scratch/bundles"
test "$(find "$unpacked" -name '*.kext' -type d | wc -l | tr -d ' ')" = 9
grep -F 'XenonFramebufferTest - fish.goldfish64.osx360.XenonFramebufferTest (0.0.4)' "$scratch/bundles" >/dev/null
/sbin/kextload -n -t -z "$unpacked/XenonFramebufferTest.kext"

check_md5 "$old_md5" "$archive"
cp -n "$archive" "$backup"
check_md5 "$old_md5" "$backup"
sync
mv -f "$temp" "$archive"
temp=
sync

verify_boot_files
check_md5 "$old_md5" "$backup"
check_md5 "$new_md5" "$archive"
echo "Verified active v4 archive and preserved boot files on $boot."
