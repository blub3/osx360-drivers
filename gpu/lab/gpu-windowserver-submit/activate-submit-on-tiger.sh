#!/bin/sh
set -eu
cd "$(dirname "$0")"
../gpu-cgl-discovery/tiger-ssh /bin/sh <<'REMOTE'
set -eu
export LC_ALL=C
boot=/Volumes/OSX360BOOT
cd "$boot"
test "$(pwd -P)" = "$boot"
old=5790ac8b037422bf101f57fd9e51c55b
new=6d5c258b4b5adb7af7f78c35c932bff9
config=kboot-gpu-windowserver-submit.conf
backup=kboot.conf.before-gpuwssubmit.bak
bundle=GLDriverXenosNative.bundle/Contents
installed=/System/Library/Extensions/$bundle
payload=XenosNativeGLDTest
verify() {
    actual=$(md5 -q "$1")
    if [ "$actual" != "$2" ]; then
        echo "Unexpected checksum for $1 ($actual)" >&2
        exit 1
    fi
}
verify "$installed/Info.plist" b67cb572421876ce39a9213d7800ae8f
verify "$installed/MacOS/GLDriverXenosNative" 653ae06ff70d3e16e62f9f0c6cbfc478
test "$(stat -f "%Su:%Sg" "$installed/MacOS/GLDriverXenosNative")" = root:wheel
verify "$payload/$bundle/Info.plist" b67cb572421876ce39a9213d7800ae8f
verify "$payload/$bundle/MacOS/GLDriverXenosNative" 653ae06ff70d3e16e62f9f0c6cbfc478
verify "$payload/XenosNativeGLDCheck" 8643dc3d9ea80f3fbac8b60dca0bc90e
verify "$payload/RunXenosNativeGLD.sh" 44fd85ed45e25378f80f2a75f43eb187
verify XenosDisplaySurfaceTest/XenosDisplaySurfaceCheck 83f7fc8bb02c198b3a87a92f2c180683
verify XenosDisplaySurfaceTest/RunXenosDisplaySurface.sh 992258307da8a3ff8f7e380a35b8d8f5
verify XenosDisplaySurfaceTest/TestNotes.txt c55671ac78afa87d89443aa6722a415a
verify XenosWSS.mkext c717c0c0606b19b4eb13b48a403e107f
verify openbios-xbox360-gpu-display-surface.elf 6e6f5ec3ac7e547f4146f61b2cd05406
verify kboot.conf.before-gpunativegld.bak 6f70043baec8ea1b5280136cf16856be
installed_ws=/System/Library/Extensions/GLDriverXenosWSProbe.bundle/Contents
ws=XenosWindowServerSubmitTest
verify "$installed_ws/Info.plist" 0360a00c8eb911aeb51258f3077c5185
verify "$installed_ws/MacOS/GLDriverXenosWSProbe" d3ec0ee7b5cc2013e0c256c0d9c742f1
test "$(stat -f "%Su:%Sg" "$installed_ws/MacOS/GLDriverXenosWSProbe")" = root:wheel
verify "$ws/GLDriverXenosWSProbe.bundle/Contents/Info.plist" 0360a00c8eb911aeb51258f3077c5185
verify "$ws/GLDriverXenosWSProbe.bundle/Contents/MacOS/GLDriverXenosWSProbe" d3ec0ee7b5cc2013e0c256c0d9c742f1
verify "$ws/XenosWindowServerSubmitCheck" 1a0ae9ea902f9b778fb7046353b643da
verify "$ws/RunXenosWindowServerSubmit.sh" 5003bb79f752b2b2f0f02132020bd8e1
verify "$ws/TestNotes.txt" 55da42ec69d3dde34a817e4da726e304
verify XenosWSP.mkext 18663e43a5af0009a381b562cb5ca66d
verify openbios-xbox360-gpu-windowserver-submit.elf 746b948434ee1eb0f6a56876cb452f8c
verify kboot.conf.before-gpudisplaysurface.bak 228691583a418df3e68ce58b5c8adca9
verify "$config" "$new"
verify kboot.conf "$old"
verify XenosGLD.mkext cfc64a836626cfdef2793a0ad17f8da3
verify openbios-xbox360-gpu-native-gld.elf d02059c9886d38f103e85f382bab3b72
verify XenosQER.mkext 4ef8f13100a98b993d9896cd709f4e38
verify XenosCGL.mkext 8ac34c28a1cc94f3ccd7bf9be6eeaa64
verify NativeHD.mkext b99d4be9377e3e7965bb3e88d5f1ff0e
verify openbios-xbox360-native-hdmi-test.elf ca72c036347285e3fe77b58d3b2c93c6
verify openbios-xbox360.720p-hdmi.elf.bak 2ce8cf67dd57d0ef8b68beaa342e051a
verify kboot.conf.before-gpuxenoswsretry.bak 2a15e64f7e185e0790d5780b578fb016
grep -Fx 'default=gpuwssubmit' "$config"
for profile in gpuwssubmit gpudisplaysurface gpunativegld gpuxenoswsretry gpucgloffscreen hdminative; do
    grep -q "^${profile}=" "$config"
done

scratch=$(mktemp -d /tmp/xenos-ws-submit-activate.XXXXXXXX)
temp=
trap 'test -z "$temp" || rm -f "$temp"; rm -rf "$scratch"' 0
trap 'exit 1' HUP INT TERM
protected_checksums() {
    find . \( -path './kboot.conf' -o -path "./$backup" -o -path './.kboot-gpuwssubmit.*' \) \
        -prune -o -type f -print0 | xargs -0 md5 | sort
}
protected_checksums > "$scratch/before"
if ! test -e "$backup"; then
    cp -n kboot.conf "$backup"
fi
verify "$backup" "$old"
verify kboot.conf "$old"
temp=$(mktemp "$boot/.kboot-gpuwssubmit.XXXXXXXX")
cat "$config" > "$temp"
verify "$temp" "$new"
sync
mv -f "$temp" kboot.conf
sync
verify kboot.conf "$new"
verify "$backup" "$old"
protected_checksums > "$scratch/after"
cmp "$scratch/before" "$scratch/after"
printf 'Verified checksums for %s protected files.\n' "$(wc -l < "$scratch/after" | tr -d ' ')"
grep -E '^default=|^(gpuwssubmit|gpudisplaysurface|gpunativegld|gpuxenoswsretry|gpucgloffscreen|hdminative)=' kboot.conf
printf 'previous_boot=%s\n' "$(/usr/sbin/sysctl -n kern.boottime)"
REMOTE
