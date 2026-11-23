set -eu
boot=/Volumes/OSX360BOOT
cd "$boot"
test "$(pwd -P)" = "$boot"
test -w "$boot"

check_md5() {
    actual=$(/sbin/md5 -q "$2")
    test "$actual" = "$1" || {
        echo "Checksum mismatch for $2: $actual" >&2
        exit 1
    }
}

check_working_boot() {
    check_md5 a95a04400a7483e3cffe54b76b8197a7 openbios-xbox360.elf
    check_md5 c9fe3cf65d40779be40b68065e5b14e9 Xbox360.mkext
    check_md5 2ce8cf67dd57d0ef8b68beaa342e051a openbios-xbox360.720p-hdmi.elf.bak
    check_md5 937e000a82e6cabc475555f871a85c13 kboot.conf
}

elf=openbios-xbox360-framebuffer-test.elf
mkext=XenosFB.mkext
config=kboot-framebuffer-test.conf
backup=kboot-working.conf.bak
check_working_boot
for file in "$elf" "$mkext" "$config" "$backup"; do
    if test -e "$file"; then
        echo "$file already exists" >&2
        exit 1
    fi
done

temp=$(mktemp -d /tmp/xenos-fb-boot.XXXXXXXX)
trap 'rm -rf "$temp"' EXIT
/usr/bin/tar -xf - -C "$temp"
check_md5 97463fc0ed55968ef041a0369f149cbf "$temp/$elf"
check_md5 a05186c53fbf0c7d6d3004da6cba33ba "$temp/$mkext"
check_md5 36487a07600bfd58dba67d0566d1008d "$temp/$config"

mkdir "$temp/unpacked"
/usr/sbin/mkextunpack -v -a ppc -d "$temp/unpacked" "$temp/$mkext"
test "$(find "$temp/unpacked" -name '*.kext' -type d | wc -l | tr -d ' ')" = 9
test -d "$temp/unpacked/XenonFramebufferTest.kext"
test -d "$temp/unpacked/IOGraphicsFamily.kext"

cp -n "$temp/$elf" "$temp/$mkext" "$temp/$config" "$boot/"
cp -n kboot.conf "$backup"
sync
check_working_boot
check_md5 97463fc0ed55968ef041a0369f149cbf "$elf"
check_md5 a05186c53fbf0c7d6d3004da6cba33ba "$mkext"
check_md5 36487a07600bfd58dba67d0566d1008d "$config"
check_md5 937e000a82e6cabc475555f871a85c13 "$backup"
echo "Verified staged test and working boot files on $boot"
