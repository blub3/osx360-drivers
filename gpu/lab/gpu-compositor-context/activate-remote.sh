set -eu
export LC_ALL=C
boot=/Volumes/OSX360BOOT
/sbin/mount | grep -Fq " on $boot ("
cd "$boot"
test "$(pwd -P)" = "$boot"
old=1c9b0443b89ba18117700c59b2e6e577
new=d4e5729d0e58cd7b12194d3469f59509
config=kboot-gpu-compositor-context.conf
backup=kboot.conf.before-gpucompositor.bak
verify() {
    actual=$(md5 -q "$2")
    if [ "$actual" != "$1" ]; then
        echo "Checksum mismatch for $2: $actual" >&2
        exit 1
    fi
}
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
verify 19b0af3a113e5fd7dd42e7302f0fc9df "./XenosCompositorContextTest/RunXenosCompositorContext.sh"
verify 1d13710c7771021b6d4bd50db57580e0 "./XenosWCC.mkext"
verify 3cae3354bb4902e933cf672f6d7ce728 "./XenosCompositorContextTest/RunXenosCompositorRepaint.sh"
verify 477f768ca2520245964f362c6477aa1e "./XenosCompositorContextTest/GLDriverXenosCompositor.bundle/Contents/MacOS/GLDriverXenosCompositor"
verify 63f961d214d4ee5be9f77a6e30d83b0a "./XenosCompositorContextTest/XenosCompositorRepaintCheck"
verify 761778380b3f2cc39e440eff7475ca3a "./XenosCompositorContextTest/TestNotes.txt"
verify 7a6ed9d448391eb911a124abb4d4edd8 "./openbios-xbox360-gpu-compositor-context.elf"
verify 87a3eded59987d704dafc00563161b02 "./XenosCompositorContextTest/XenosCompositorContextCheck"
verify a5a58a46e372dc78c971b59fbacb041c "./XenosCompositorContextTest/GLDriverXenosCompositor.bundle/Contents/Info.plist"
verify d4e5729d0e58cd7b12194d3469f59509 "./kboot-gpu-compositor-context.conf"
contents=/System/Library/Extensions/GLDriverXenosCompositor.bundle/Contents
verify a5a58a46e372dc78c971b59fbacb041c "$contents/Info.plist"
verify 477f768ca2520245964f362c6477aa1e "$contents/MacOS/GLDriverXenosCompositor"
test "$(stat -f "%Su:%Sg:%Lp" "$contents/MacOS/GLDriverXenosCompositor")" = root:wheel:755
test ! -e XenosCompositorContextTest/repaint-probe.arm
grep -Fx 'default=gpucompositor' "$config"
grep -Fx 'gpucompositor="uda0:/openbios-xbox360-gpu-compositor-context.elf"' "$config"

current=$(md5 -q kboot.conf)
case "$current" in
    "$new")
        verify "$old" "$backup"
        printf 'gpucompositor is already active.\n'
        printf 'previous_boot=%s\n' "$(/usr/sbin/sysctl -n kern.boottime)"
        exit 0
        ;;
    "$old") ;;
    *) printf 'Unexpected active boot configuration: %s\n' "$current" >&2; exit 1 ;;
esac
scratch=$(mktemp -d /tmp/xenos-compositor-activate.XXXXXXXX)
temp=
trap 'test -z "$temp" || rm -f "$temp"; rm -rf "$scratch"' 0
trap 'exit 1' HUP INT TERM
protected_checksums() {
    find . \( -path './kboot.conf' -o -path "./$backup" -o -path './.kboot-gpucompositor.*' \) \
        -prune -o -type f -print0 | xargs -0 md5 | sort
}
protected_checksums > "$scratch/before"
if ! test -e "$backup"; then
    cp -n kboot.conf "$backup"
fi
verify "$old" "$backup"
verify "$old" kboot.conf
temp=$(mktemp "$boot/.kboot-gpucompositor.XXXXXXXX")
cat "$config" > "$temp"
verify "$new" "$temp"
sync
mv -f "$temp" kboot.conf
sync
verify "$new" kboot.conf
verify "$old" "$backup"
protected_checksums > "$scratch/after"
cmp "$scratch/before" "$scratch/after"
printf 'Verified checksums for %s protected files.\n' "$(wc -l < "$scratch/after" | tr -d ' ')"
grep -E '^default=|^(gpucompositor|gpuwsfbretry|gpuwssubmit|hdminative)=' kboot.conf
printf 'previous_boot=%s\n' "$(/usr/sbin/sysctl -n kern.boottime)"
