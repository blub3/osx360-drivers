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
old_default=6a2f4b93150dee38b6ffd4262d047dc9
new_default=aee74a223e91c787a95b6d398fab8285
config=kboot-gpu-quad-batch-trilinear.conf
backup=kboot.conf.before-gpuquadtrilinear.bak
check_md5 "$old_default" kboot.conf
check_md5 "$old_default" kboot-gpu-quad-batch-mipmap.conf
check_md5 d5d0d3cd530e38cad535deb4a2706c82 kboot-gpu-quad-batch-bilinear-retry.conf
check_md5 d5d0d3cd530e38cad535deb4a2706c82 kboot.conf.before-gpuquadmipmap.bak
check_md5 a19a6f81b4e28111569c3e90783d0cf2 kboot-gpu-quad-batch-bilinear.conf
check_md5 a19a6f81b4e28111569c3e90783d0cf2 kboot.conf.before-bilinear-retry.bak
check_md5 34bac7c3cb547e780d0c30bc589272b8 kboot-gpu-quad-batch-render-texture.conf
check_md5 34bac7c3cb547e780d0c30bc589272b8 kboot.conf.before-gpuquadbilinear.bak
check_md5 fbfdaf0c08e94e6c9ae2d66503395ff2 kboot-gpu-quad-batch-depth.conf
check_md5 fbfdaf0c08e94e6c9ae2d66503395ff2 kboot.conf.before-gpuquadrtt.bak
check_md5 11696b2c911eb71b3e692c356ed3766c kboot-gpu-quad-batch-scissor-stencil.conf
check_md5 11696b2c911eb71b3e692c356ed3766c kboot.conf.before-gpuquaddepth.bak
check_md5 6f72a8cbc2f89194edd83db7f484f736 kboot-gpu-quad-batch-stencil-retry.conf
check_md5 6f72a8cbc2f89194edd83db7f484f736 kboot.conf.before-gpuquadclip.bak
check_md5 48556223bb33f3b4a2a4e678e00c3714 kboot-gpu-quad-batch-stencil.conf
check_md5 48556223bb33f3b4a2a4e678e00c3714 kboot.conf.before-stencil-retry.bak
check_md5 1d26b704eda4c232b35d199d76c7db35 kboot-gpu-quad-batch-scissor.conf
check_md5 1d26b704eda4c232b35d199d76c7db35 kboot.conf.before-gpuquadstencil.bak
check_md5 c092723515ab7afb2d7f8ae6ff9de0a2 kboot.conf.before-gpuquadscissor.bak
check_md5 c092723515ab7afb2d7f8ae6ff9de0a2 kboot-gpu-quad-batch-blend.conf
check_md5 26ef82029e50eb22c73f11bf87600cd0 kboot.conf.before-gpuquadblend.bak
check_md5 26ef82029e50eb22c73f11bf87600cd0 kboot-gpu-quad-batch.conf
check_md5 ae85c256b66b15c0f6232dc84de745e8 kboot.conf.before-gpuquadbat.bak
check_md5 ae85c256b66b15c0f6232dc84de745e8 kboot-gpu-quad-rotate.conf
check_md5 cf4868b4ff5125d2321cfa0b9a605f92 kboot.conf.before-gpuquadrot.bak
check_md5 cf4868b4ff5125d2321cfa0b9a605f92 kboot-gpu-quad-geometry.conf
check_md5 "$new_default" "$config"
grep -Fx 'default=gpuquadtrilinear' "$config" >/dev/null
grep -Fx 'gpuquadbilinear="uda0:/openbios-xbox360-gpu-quad-batch-bilinear-retry.elf"' "$config" >/dev/null
grep -Fx 'gpuquadmipmap="uda0:/openbios-xbox360-gpu-quad-batch-mipmap.elf"' "$config" >/dev/null
grep -Fx 'gpuquadtrilinear="uda0:/openbios-xbox360-gpu-quad-batch-trilinear.elf"' "$config" >/dev/null
check_md5 a24155aede6d426cb00ee716c166642f XenosQTR.mkext
check_md5 01b9c4bcfdabfcde9c2f5f0144e4010a openbios-xbox360-gpu-quad-batch-trilinear.elf
check_md5 19dc51959fb6987ba60b16deb96afea3 XenosQuadBatchTrilinear
check_md5 c62ee62a19a6677dc943c97f11a71fa7 XenosQMP.mkext
check_md5 6977a92207cbdeedfe41a3e506ba38d1 openbios-xbox360-gpu-quad-batch-mipmap.elf
check_md5 e9a00ddd30f3240935db9bce8e1c544e XenosQuadBatchMipmap
check_md5 4098eb428552877d89b75357bab36b8b XenosQBR.mkext
check_md5 bc2dead6c2089884a8e5077a5ad42f17 openbios-xbox360-gpu-quad-batch-bilinear-retry.elf
check_md5 4a84a1585ea506ed761aaf46011056cb XenosQuadBatchBilinearRetry
check_md5 d75341db1e87d23dd0ec3a619d75d41f XenosQBL.mkext
check_md5 70904fda420c3b315ff780c6877a3e7a openbios-xbox360-gpu-quad-batch-bilinear.elf
check_md5 4a84a1585ea506ed761aaf46011056cb XenosQuadBatchBilinear
check_md5 29a246dcd134e1a5db83db4f4761620c XenosQRT.mkext
check_md5 67abfc57dc508e9b6e2500aece6c8743 openbios-xbox360-gpu-quad-batch-render-texture.elf
check_md5 cda99cdbd7a7ee7c3a8c96d559623345 XenosQuadBatchRenderTexture
check_md5 a1c74314223a3133765a57e44dfd7329 XenosQDP.mkext
check_md5 c3d27a1e66842b52df9a7e6b3400ffa3 openbios-xbox360-gpu-quad-batch-depth.elf
check_md5 6c69d92a88e232ec22826b9969d24ee7 XenosQuadBatchDepth
check_md5 28fe0098ab8395dce216edfc1e25cf9d XenosQCS.mkext
check_md5 ad074654547645c94427c6d31bbfbc08 openbios-xbox360-gpu-quad-batch-scissor-stencil.elf
check_md5 88c5c86fdc00f3e6b2606bd7777cb719 XenosQuadBatchScissorStencil
check_md5 e0db7613867b6ff2925abb91ac591013 XenosQSR.mkext
check_md5 94e5abf84a91ad12fe4673929735b23e openbios-xbox360-gpu-quad-batch-stencil-retry.elf
check_md5 097457470d3c71b522604a6a363ec4c4 XenosQuadBatchStencilRetry
check_md5 18505a3a0ab5665c8e273e53ec82747d XenosQST.mkext
check_md5 ae04de844947c7641b591aa4d10bb51e openbios-xbox360-gpu-quad-batch-stencil.elf
check_md5 097457470d3c71b522604a6a363ec4c4 XenosQuadBatchStencil
check_md5 bc05cb40e68353143295aab458db0f17 XenosQSC.mkext
check_md5 e28485148878c31e9801545a8bdc0197 openbios-xbox360-gpu-quad-batch-scissor.elf
check_md5 0d947063ed567c09020545d98fab2526 XenosQuadBatchScissor
check_md5 bd8b18357f220b11a57066687b747b24 XenosQAB.mkext
check_md5 07ef5c42f804a7b89570839ba841145b openbios-xbox360-gpu-quad-batch-blend.elf
check_md5 613a22cd6d22271fce8f5a4178668d0b XenosQuadBatchBlend
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

scratch=$(mktemp -d /tmp/xenos-activate-quadtrilinear.XXXXXXXX)
temp=
trap 'if test -n "$temp"; then rm -f "$temp"; fi; rm -rf "$scratch"' EXIT
boot_manifest() {
    for path in ./* ./.[!.]* ./..?*; do
        case "$path" in
            ./kboot.conf|"./$backup"|./.kboot-gpuquadtrilinear.*) continue ;;
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
temp=$(mktemp "$boot/.kboot-gpuquadtrilinear.XXXXXXXX")
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
grep -E '^default=|^gpuquad(mipmap|trilinear)=' kboot.conf
printf 'Verified rollback config %s and %s pre-existing files.\n' "$backup" "$(wc -l < "$scratch/after" | tr -d ' ')"
/usr/sbin/sysctl -n kern.boottime
