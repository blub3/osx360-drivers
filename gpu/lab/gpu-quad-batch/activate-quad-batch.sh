set -eu
boot=/Volumes/OSX360BOOT
cd "$boot"
test "$(pwd -P)" = "$boot"
/sbin/mount | grep -F ' on /Volumes/OSX360BOOT (' >/dev/null

check_md5() {
    actual=$(/sbin/md5 -q "$2")
    if test "$actual" != "$1"; then
        printf 'Checksum mismatch for %s (%s)\n' "$2" "$actual" >&2
        exit 1
    fi
}
old_default=ae85c256b66b15c0f6232dc84de745e8
new_default=26ef82029e50eb22c73f11bf87600cd0
config=kboot-gpu-quad-batch.conf
backup=kboot.conf.before-gpuquadbat.bak
check_md5 "$old_default" kboot.conf
check_md5 "$old_default" kboot-gpu-quad-rotate.conf
check_md5 cf4868b4ff5125d2321cfa0b9a605f92 kboot.conf.before-gpuquadrot.bak
check_md5 cf4868b4ff5125d2321cfa0b9a605f92 kboot-gpu-quad-geometry.conf
check_md5 "$new_default" "$config"
grep -Fx 'default=gpuquadbat' "$config" >/dev/null
grep -Fx 'gpuquadbat="uda0:/openbios-xbox360-gpu-quad-batch.elf"' "$config" >/dev/null
check_md5 aca071d8dfb74dc9e8d9295e53ad6aeb XenosBAT.mkext
check_md5 40add9eb84e5a6b7359a9494ca8d8773 openbios-xbox360-gpu-quad-batch.elf
check_md5 6a9a9452712ef68318722b9092caad19 XenosQuadBatch
check_md5 c2564c37186e183d7c6dd2e960d34b49 XenosROT.mkext
check_md5 0d5507ac830b633486282b6c21fbb2da openbios-xbox360-gpu-quad-rotate.elf
check_md5 b4df9dd29591a5560a9bdb25367e92d2 XenosQuadRotate
check_md5 a9f8b486795fdf7ba1bef0fb4dfb5065 XenosGEO.mkext
check_md5 54586dfaee8fcbae56a111d4540597cf XenosQuadGeometry
check_md5 6b376b64e92ddd18c7f450b28228bb23 kboot.conf.before-gpuquadgeo.bak
check_md5 0ee943014197652505395a6e19c86363 XenosQuadUpload
check_md5 000866e10c9b9dc68bcbcc28d1958e43 XenosQuad
check_md5 ca72c036347285e3fe77b58d3b2c93c6 openbios-xbox360-native-hdmi-test.elf
check_md5 b99d4be9377e3e7965bb3e88d5f1ff0e NativeHD.mkext
check_md5 2ce8cf67dd57d0ef8b68beaa342e051a openbios-xbox360.720p-hdmi.elf.bak

scratch=$(mktemp -d /tmp/xenos-activate-quadbat.XXXXXXXX)
temp=
trap 'if test -n "$temp"; then rm -f "$temp"; fi; rm -rf "$scratch"' EXIT
boot_manifest() {
    for path in ./* ./.[!.]* ./..?*; do
        case "$path" in
            ./kboot.conf|"./$backup"|./.kboot-gpuquadbat.*) continue ;;
        esac
        if test -f "$path"; then
            /sbin/md5 "$path"
        fi
    done
}
boot_manifest > "$scratch/before"
if ! test -e "$backup"; then
    cp -n kboot.conf "$backup"
fi
check_md5 "$old_default" "$backup"
temp=$(mktemp "$boot/.kboot-gpuquadbat.XXXXXXXX")
cat "$config" > "$temp"
check_md5 "$new_default" "$temp"
check_md5 "$old_default" kboot.conf
sync
mv -f "$temp" kboot.conf
temp=
sync
check_md5 "$new_default" kboot.conf
check_md5 "$old_default" "$backup"
boot_manifest > "$scratch/after"
cmp "$scratch/before" "$scratch/after"
grep -E '^default=|^gpuquadbat=' kboot.conf
printf 'Verified rollback config %s and %s pre-existing files.\n' "$backup" "$(wc -l < "$scratch/after" | tr -d ' ')"
sysctl kern.boottime
