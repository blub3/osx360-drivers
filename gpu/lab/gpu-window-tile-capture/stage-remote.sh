set -eu
export LC_ALL=C
boot=/Volumes/OSX360BOOT
/sbin/mount | grep -Fq " on $boot ("
cd "$boot"
test "$(pwd -P)" = "$boot"
verify() {
    actual=$(md5 -q "$2")
    if [ "$actual" != "$1" ]; then
        echo "Checksum mismatch for $2: $actual" >&2
        exit 1
    fi
}
verify 1aedad97fcc51c11b1215eb5ebaa772a kboot.conf
grep -Fx 'default=gpuringbuf' kboot.conf
verify d4e5729d0e58cd7b12194d3469f59509 kboot.conf.before-gpucompbootfix.bak
verify c76086865a738b203ebe3aab6f628dd5 kboot.conf.before-gpufbblit.bak
verify 307278fc63c1ba863a75d313af0ba14b kboot-gpu-fb-blit-surface.conf
verify a1f4dee1cea39d416ec981989d254556 XWBL.MKE
verify 06b186cf2937bd8a03af7f43c525ed6e BLITSURF.ELF
verify 14cf4a695008d3ff43a00b3ed1303545 /System/Library/Extensions/XenosLegacyBlit.plugin/Contents/Info.plist
verify f3f4a7603891bd58198174f35741ab05 /System/Library/Extensions/XenosLegacyBlit.plugin/Contents/MacOS/XenosLegacyBlit
verify c76086865a738b203ebe3aab6f628dd5 kboot.conf.before-gpufbblitbeam.bak
verify 92b5d269d906ca20b85264513d97e5b6 kboot-gpu-fb-blit-beam.conf
verify 320b36a69712ad437d044017d633d45c XWBM.MKE
verify e5e98da3e48d8d78b04a2195481b0237 BLITBEAM.ELF
verify 89e70b3a0f25e65754d278c6c72ba290 /System/Library/Extensions/XenosBeamBlit.plugin/Contents/Info.plist
verify 7e332bed576340018854a3e3b7d41ba6 /System/Library/Extensions/XenosBeamBlit.plugin/Contents/MacOS/XenosBeamBlit
verify 92b5d269d906ca20b85264513d97e5b6 kboot.conf.before-gpuwinrepaint.bak
verify 348ca1649e90bea8a13541525f9ac0d6 kboot-gpu-window-repaint.conf
verify c45190432a777666157c3d7a6e346f0e XWRP.MKE
verify b0e25ffc8fecbd7a0e55ab9300122e24 WINPAINT.ELF
verify a9a392b3cd1d0e545bb591371ae20d0d /System/Library/Extensions/XenosWinBlit.plugin/Contents/Info.plist
verify 4e22f02494f032f7a815d3e15ccbd401 /System/Library/Extensions/XenosWinBlit.plugin/Contents/MacOS/XenosWinBlit
verify 348ca1649e90bea8a13541525f9ac0d6 kboot.conf.before-gpuwinretry.bak
verify a9c544c6bf6ab532695cc776f7804c13 kboot-gpu-window-retry.conf
verify 8de0a6ef81b355802c4a6d6a313ec593 XWRR.MKE
verify a113aae33d1bdd6199be768fdc7c8a5b WINRETRY.ELF
verify 7085f52896edcff439f0a27a89042a2f /System/Library/Extensions/XenosWinFix.plugin/Contents/Info.plist
verify 71f7678814b8fd65017ad2e320b1029f /System/Library/Extensions/XenosWinFix.plugin/Contents/MacOS/XenosWinFix
verify a9c544c6bf6ab532695cc776f7804c13 kboot.conf.before-gpuwinmap.bak
verify 78be67e139d89a01fc952d5960c7a149 kboot-gpu-window-map.conf
verify 1b39f90c3118bd04aca9e378a10806e5 XWFM.MKE
verify 9b046f3fbcc557cb816fde87b91f1f88 WINMAP.ELF
verify ab9805622ae2053c3a1bb2eedbd77482 /System/Library/Extensions/XenosWinMap.plugin/Contents/Info.plist
verify 7fb921f6721e1e79f6028e06bc932981 /System/Library/Extensions/XenosWinMap.plugin/Contents/MacOS/XenosWinMap
verify 78be67e139d89a01fc952d5960c7a149 kboot.conf.before-gpuwinfull.bak
verify 8500642a077e781b85a22024b6a049f4 kboot-gpu-window-full.conf
verify cf17e0d3012a8748a3d9f60c44a24096 XWFR.MKE
verify f362480e8cd318e5791089c6651abb3c WINFULL.ELF
verify 025b97daec94c65f58ac4c1bae878315 /System/Library/Extensions/XenosWinAll.plugin/Contents/Info.plist
verify dcb1099719cb4b697cd8b977c3f81675 /System/Library/Extensions/XenosWinAll.plugin/Contents/MacOS/XenosWinAll
verify 8500642a077e781b85a22024b6a049f4 kboot.conf.before-gpuwinbatch.bak
verify c8d3080f6df7a8e63aa6c66a2a5a1db3 kboot-gpu-window-batch.conf
verify abb54eca183cb316bd4e8cdad7dd8ac1 XWBT.MKE
verify 04cfc858e927a09f4d922e0d07ead5c4 WINBATCH.ELF
verify 1716b0585eb6f6fd962d8b27beaed664 /System/Library/Extensions/XenosWinBatch.plugin/Contents/Info.plist
verify 892205a83921f3a6fe4edc7330dadf1f /System/Library/Extensions/XenosWinBatch.plugin/Contents/MacOS/XenosWinBatch
verify 9c5d30f22fb57d56388a25d53397813f XenosWindowBatchTest/RunXenosWindowBatch.sh
verify 55dffa91c5639458fc2f16d0823fd0b9 XenosWindowBatchTest/XenosWindowRepaintCheck
test ! -e XenosWindowBatchTest/repaint-window.arm
test ! -e XenosWindowBatchTest/repaint-window.lock
verify 3d5be96c3df8c4c3cdfc514c7c637ba1 CMPFIX.ELF
verify c603562c9452658c09f5e25db66ff936 XWCF.MKE
verify 1c9b0443b89ba18117700c59b2e6e577 kboot.conf.before-gpucompositor.bak
verify 1d13710c7771021b6d4bd50db57580e0 XenosWCC.mkext
verify d4e5729d0e58cd7b12194d3469f59509 kboot.conf.before-gpucompretry.bak
verify cf2b0b281476ebd922e9d07dc979c7e0 XenosWCR.mkext
verify 539b08d0a314f0e36bb081ea9b96d8d6 kboot-gpu-compositor-retry.conf
verify 10cb161defa1d9b11f544ef5bf548f93 openbios-xbox360-gpu-compositor-retry.elf
verify 67af5267dc7340d3c5dfc99e488cd404 /System/Library/Extensions/GLDriverXenosCompositorRetry.bundle/Contents/MacOS/GLDriverXenosCompositorRetry
verify 7a6ed9d448391eb911a124abb4d4edd8 openbios-xbox360-gpu-compositor-context.elf
verify 477f768ca2520245964f362c6477aa1e /System/Library/Extensions/GLDriverXenosCompositor.bundle/Contents/MacOS/GLDriverXenosCompositor
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
verify c8d3080f6df7a8e63aa6c66a2a5a1db3 kboot.conf.before-gpuringfix.bak
verify cb1e1ca86481c56bdf4a67b5e57ae42e kboot-gpu-ring-startup.conf
verify febe27bec26dec650b33ad0fb6a68816 XWRF.MKE
verify d21ff4d61f650ad51a24df598a56539a RINGFIX.ELF
verify e2716e3f3186181485a6217b6d13979a XenosRingStartupTest/CheckXenosRingStartup.sh
verify cb1e1ca86481c56bdf4a67b5e57ae42e kboot.conf.before-gpuringbuf.bak
verify 1aedad97fcc51c11b1215eb5ebaa772a kboot-gpu-ring-buffer.conf
verify 035a699f914d23541fa2709107f065dd XWRB.MKE
verify 518e5936eb46df7789de28cf5a3e01f7 RINGBUF.ELF
verify 147a3151947f2ec140f1bdb15e3f5d63 XenosRingBufferTest/CheckXenosRingBuffer.sh
test ! -e /System/Library/Extensions/XenosWinTile.plugin
echo "Verified active profile, rollback, native HDMI, and 720p backup before staging."
archive=XWTC.MKE
firmware=WINTILE.ELF
config=kboot-gpu-window-tile.conf
testdir=XenosWindowTileTest
for name in "$archive" "$firmware" "$config" "$testdir"; do
    test ! -e "$name"
done
scratch=$(mktemp -d /tmp/xenos-window-tile-stage.XXXXXXXX)
incoming=$(mktemp -d "$boot/.xenos-window-tile-incoming.XXXXXXXX")
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
printf 'Verified %s pre-existing files and staged gpuwintile.\n' \
    "$(wc -l < "$scratch/after" | tr -d ' ')"
grep '^default=' kboot.conf "$config"
