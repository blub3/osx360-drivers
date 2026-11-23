#!/bin/sh
set -eu
export LC_ALL=C
boot=/Volumes/OSX360BOOT
/sbin/mount | grep -Fq " on $boot ("
cd "$boot"
test "$(pwd -P)" = "$boot"

verify() {
    actual=$(md5 -q "$2")
    if [ "$actual" != "$1" ]; then
        echo "Unexpected checksum for $2: $actual" >&2
        exit 1
    fi
}
active=c76086865a738b203ebe3aab6f628dd5
restored=d4e5729d0e58cd7b12194d3469f59509
backup=kboot.conf.before-gpucompbootfix.bak
verify "$restored" "$backup"
verify "$restored" kboot-gpu-compositor-context.conf
verify "$active" kboot-gpu-compositor-bootfix.conf
verify 539b08d0a314f0e36bb081ea9b96d8d6 kboot-gpu-compositor-retry.conf
verify d4e5729d0e58cd7b12194d3469f59509 kboot.conf.before-gpucompretry.bak
verify 3d5be96c3df8c4c3cdfc514c7c637ba1 CMPFIX.ELF
verify c603562c9452658c09f5e25db66ff936 XWCF.MKE
verify 1c9b0443b89ba18117700c59b2e6e577 kboot.conf.before-gpucompositor.bak
verify 1d13710c7771021b6d4bd50db57580e0 XenosWCC.mkext
verify 7a6ed9d448391eb911a124abb4d4edd8 openbios-xbox360-gpu-compositor-context.elf
verify cf2b0b281476ebd922e9d07dc979c7e0 XenosWCR.mkext
verify 10cb161defa1d9b11f544ef5bf548f93 openbios-xbox360-gpu-compositor-retry.elf
verify b99d4be9377e3e7965bb3e88d5f1ff0e NativeHD.mkext
verify ca72c036347285e3fe77b58d3b2c93c6 openbios-xbox360-native-hdmi-test.elf
verify 2ce8cf67dd57d0ef8b68beaa342e051a openbios-xbox360.720p-hdmi.elf.bak

current=$(md5 -q kboot.conf)
if [ "$current" != "$restored" ]; then
    verify "$active" kboot.conf
    temp=$(mktemp "$boot/.kboot-gpucompositor-restore.XXXXXXXX")
    trap 'rm -f "$temp"' 0
    trap 'exit 1' HUP INT TERM
    cat "$backup" > "$temp"
    verify "$restored" "$temp"
    sync
    mv -f "$temp" kboot.conf
    sync
fi
verify "$restored" kboot.conf
verify "$restored" "$backup"
grep -Fx 'default=gpucompositor' kboot.conf
echo "Restored and verified the previous gpucompositor boot configuration."
