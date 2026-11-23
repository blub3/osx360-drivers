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
    for path in ./* ./.[!.]* ./..?*; do
        case "$path" in
            "./$archive"|"./$elf"|"./$config"|"./$app") continue ;;
        esac
        if test -f "$path"; then
            md5 "$path"
        fi
    done
}

archive=XenosSRF.mkext
elf=openbios-xbox360-gpu-reusable-surface.elf
config=kboot-gpu-reusable-surface.conf
app=XenosSurface
verify cf4868b4ff5125d2321cfa0b9a605f92 kboot-gpu-quad-geometry.conf
verify ce1dbc90d3be77da13697d0644c3ca1f kboot.conf
verify ce1dbc90d3be77da13697d0644c3ca1f kboot-gpu-quad-batch-anisotropic.conf
verify aa4624a43fc06b7b0dde0954d9028392 openbios-xbox360-gpu-quad-batch-anisotropic.elf
verify 4140c9e6c01e3671774ae9eb3042c67f XenosQAF.mkext
verify fea8cf450af6f3d580ed8e7c86f9552a XenosQuadBatchAnisotropic
verify 40dcf17a835dc47ec21dc92d7d79fa81 kboot.conf.before-gpuquadaniso.bak
verify 40dcf17a835dc47ec21dc92d7d79fa81 kboot-gpu-quad-batch-trilinear-retry.conf
verify 9346b70c0fb7da6fcc71469777300fea openbios-xbox360-gpu-quad-batch-trilinear-retry.elf
verify d53fb664970daa133560a4a92f3d072d XenosQT2.mkext
verify 716f2efe129960f4701e31ac57d6416e XenosQuadBatchTrilinearRetry
verify aee74a223e91c787a95b6d398fab8285 kboot.conf.before-gpuquadtriretry.bak
verify aee74a223e91c787a95b6d398fab8285 kboot-gpu-quad-batch-trilinear.conf
verify 01b9c4bcfdabfcde9c2f5f0144e4010a openbios-xbox360-gpu-quad-batch-trilinear.elf
verify a24155aede6d426cb00ee716c166642f XenosQTR.mkext
verify 19dc51959fb6987ba60b16deb96afea3 XenosQuadBatchTrilinear
verify 6a2f4b93150dee38b6ffd4262d047dc9 kboot.conf.before-gpuquadtrilinear.bak
verify 6a2f4b93150dee38b6ffd4262d047dc9 kboot-gpu-quad-batch-mipmap.conf
verify 6977a92207cbdeedfe41a3e506ba38d1 openbios-xbox360-gpu-quad-batch-mipmap.elf
verify c62ee62a19a6677dc943c97f11a71fa7 XenosQMP.mkext
verify e9a00ddd30f3240935db9bce8e1c544e XenosQuadBatchMipmap
verify d5d0d3cd530e38cad535deb4a2706c82 kboot.conf.before-gpuquadmipmap.bak
verify d5d0d3cd530e38cad535deb4a2706c82 kboot-gpu-quad-batch-bilinear-retry.conf
verify bc2dead6c2089884a8e5077a5ad42f17 openbios-xbox360-gpu-quad-batch-bilinear-retry.elf
verify 4098eb428552877d89b75357bab36b8b XenosQBR.mkext
verify 4a84a1585ea506ed761aaf46011056cb XenosQuadBatchBilinearRetry
verify a19a6f81b4e28111569c3e90783d0cf2 kboot.conf.before-bilinear-retry.bak
verify a19a6f81b4e28111569c3e90783d0cf2 kboot-gpu-quad-batch-bilinear.conf
verify 34bac7c3cb547e780d0c30bc589272b8 kboot.conf.before-gpuquadbilinear.bak
verify 70904fda420c3b315ff780c6877a3e7a openbios-xbox360-gpu-quad-batch-bilinear.elf
verify d75341db1e87d23dd0ec3a619d75d41f XenosQBL.mkext
verify 4a84a1585ea506ed761aaf46011056cb XenosQuadBatchBilinear
verify 34bac7c3cb547e780d0c30bc589272b8 kboot-gpu-quad-batch-render-texture.conf
verify fbfdaf0c08e94e6c9ae2d66503395ff2 kboot.conf.before-gpuquadrtt.bak
verify 67abfc57dc508e9b6e2500aece6c8743 openbios-xbox360-gpu-quad-batch-render-texture.elf
verify 29a246dcd134e1a5db83db4f4761620c XenosQRT.mkext
verify cda99cdbd7a7ee7c3a8c96d559623345 XenosQuadBatchRenderTexture
verify fbfdaf0c08e94e6c9ae2d66503395ff2 kboot-gpu-quad-batch-depth.conf
verify 11696b2c911eb71b3e692c356ed3766c kboot.conf.before-gpuquaddepth.bak
verify c3d27a1e66842b52df9a7e6b3400ffa3 openbios-xbox360-gpu-quad-batch-depth.elf
verify a1c74314223a3133765a57e44dfd7329 XenosQDP.mkext
verify 6c69d92a88e232ec22826b9969d24ee7 XenosQuadBatchDepth
verify 11696b2c911eb71b3e692c356ed3766c kboot-gpu-quad-batch-scissor-stencil.conf
verify 6f72a8cbc2f89194edd83db7f484f736 kboot.conf.before-gpuquadclip.bak
verify ad074654547645c94427c6d31bbfbc08 openbios-xbox360-gpu-quad-batch-scissor-stencil.elf
verify 28fe0098ab8395dce216edfc1e25cf9d XenosQCS.mkext
verify 88c5c86fdc00f3e6b2606bd7777cb719 XenosQuadBatchScissorStencil
verify 6f72a8cbc2f89194edd83db7f484f736 kboot-gpu-quad-batch-stencil-retry.conf
verify 48556223bb33f3b4a2a4e678e00c3714 kboot.conf.before-stencil-retry.bak
verify 94e5abf84a91ad12fe4673929735b23e openbios-xbox360-gpu-quad-batch-stencil-retry.elf
verify e0db7613867b6ff2925abb91ac591013 XenosQSR.mkext
verify 097457470d3c71b522604a6a363ec4c4 XenosQuadBatchStencilRetry
verify 48556223bb33f3b4a2a4e678e00c3714 kboot-gpu-quad-batch-stencil.conf
verify 1d26b704eda4c232b35d199d76c7db35 kboot.conf.before-gpuquadstencil.bak
verify ae04de844947c7641b591aa4d10bb51e openbios-xbox360-gpu-quad-batch-stencil.elf
verify 18505a3a0ab5665c8e273e53ec82747d XenosQST.mkext
verify 097457470d3c71b522604a6a363ec4c4 XenosQuadBatchStencil
verify 1d26b704eda4c232b35d199d76c7db35 kboot-gpu-quad-batch-scissor.conf
verify c092723515ab7afb2d7f8ae6ff9de0a2 kboot.conf.before-gpuquadscissor.bak
verify e28485148878c31e9801545a8bdc0197 openbios-xbox360-gpu-quad-batch-scissor.elf
verify bc05cb40e68353143295aab458db0f17 XenosQSC.mkext
verify 0d947063ed567c09020545d98fab2526 XenosQuadBatchScissor
verify c092723515ab7afb2d7f8ae6ff9de0a2 kboot-gpu-quad-batch-blend.conf
verify 26ef82029e50eb22c73f11bf87600cd0 kboot.conf.before-gpuquadblend.bak
verify 07ef5c42f804a7b89570839ba841145b openbios-xbox360-gpu-quad-batch-blend.elf
verify bd8b18357f220b11a57066687b747b24 XenosQAB.mkext
verify 613a22cd6d22271fce8f5a4178668d0b XenosQuadBatchBlend
verify 26ef82029e50eb22c73f11bf87600cd0 kboot-gpu-quad-batch.conf
verify ae85c256b66b15c0f6232dc84de745e8 kboot.conf.before-gpuquadbat.bak
verify 40add9eb84e5a6b7359a9494ca8d8773 openbios-xbox360-gpu-quad-batch.elf
verify aca071d8dfb74dc9e8d9295e53ad6aeb XenosBAT.mkext
verify 6a9a9452712ef68318722b9092caad19 XenosQuadBatch
verify ae85c256b66b15c0f6232dc84de745e8 kboot-gpu-quad-rotate.conf
verify cf4868b4ff5125d2321cfa0b9a605f92 kboot.conf.before-gpuquadrot.bak
verify 0d5507ac830b633486282b6c21fbb2da openbios-xbox360-gpu-quad-rotate.elf
verify c2564c37186e183d7c6dd2e960d34b49 XenosROT.mkext
verify b4df9dd29591a5560a9bdb25367e92d2 XenosQuadRotate
verify a5e5f7e11cfe69df3ba4885cf63ae465 openbios-xbox360-gpu-quad-geometry.elf
verify a9f8b486795fdf7ba1bef0fb4dfb5065 XenosGEO.mkext
verify 54586dfaee8fcbae56a111d4540597cf XenosQuadGeometry
verify 6b376b64e92ddd18c7f450b28228bb23 kboot.conf.before-gpuquadgeo.bak
verify 6b376b64e92ddd18c7f450b28228bb23 kboot-gpu-quad-upload.conf
verify 5d525f7970f2b31dcc159af3f57717d5 openbios-xbox360-gpu-quad-upload.elf
verify 7a02a7d34724787e23d3851b77193fbf XenosUPL.mkext
verify 0ee943014197652505395a6e19c86363 XenosQuadUpload
verify 33d96696024355b5b78d762182a3ff5d kboot.conf.before-gpuquadup.bak
verify 33d96696024355b5b78d762182a3ff5d kboot-gpu-user-quad.conf
verify 911fb481f22313d93b280099f63e16e7 openbios-xbox360-gpu-user-quad.elf
verify dfca12da12954882d164efaf737140a4 XenosUSR.mkext
verify 000866e10c9b9dc68bcbcc28d1958e43 XenosQuad
verify a503e6cd0fc52bc71849e5b8c87ef55a kboot.conf.before-gpuuserquad.bak
verify a503e6cd0fc52bc71849e5b8c87ef55a kboot-gpu-vblank-queue.conf
verify efa66031879c57dde7e9e37ac1f7beef openbios-xbox360-gpu-vblank-queue.elf
verify 5a1ec02685909f130ab64842a2315940 XenosVBQ.mkext
verify 7290e7f3ac3c81d2fb5622b536259e22 kboot.conf.before-gpuvbqueue.bak
verify 7290e7f3ac3c81d2fb5622b536259e22 kboot-gpu-vblank-retry.conf
verify 851cc10f9236e25592bc9bb8a84b0a5e openbios-xbox360-gpu-vblank-retry.elf
verify c4b1c4f955917157d05112d5bc942488 XenosVBR.mkext
verify 329aec17e3f504dbb463e6aca209b6f4 kboot.conf.before-gpuvbretry.bak
verify 329aec17e3f504dbb463e6aca209b6f4 kboot-gpu-vblank-flips.conf
verify 38ace2c0c35036e61879e0662d020d04 openbios-xbox360-gpu-vblank-flips.elf
verify ac268db7d875f911ab59873cc4a8c971 XenosVBL.mkext
verify 559eb6e1d781e61448fe6be5ec4dbc6b kboot.conf.before-gpuvblank.bak
verify 559eb6e1d781e61448fe6be5ec4dbc6b kboot-gpu-moving-composite.conf
verify 69cf1f90a7bdadaf7e7f3338fd5d435d openbios-xbox360-gpu-moving-composite.elf
verify 61a025e099532b7e299540bcbfbf49b5 XenosMOV.mkext
verify ddee0118e607e74df117d1698b089713 kboot.conf.before-gpumoving.bak
verify ddee0118e607e74df117d1698b089713 kboot-gpu-texture-composite.conf
verify 8486de52ceca4b0eae62f2ee5bed7182 openbios-xbox360-gpu-texture-composite.elf
verify dfa2e5d8d74dd053aee8bdc489e03aac XenosCMP.mkext
verify 9251f33fe9c72e07a43c21f55e915f86 kboot-gpu-render-texture.conf
verify 537f5152566ff701688ac793d14d24a0 openbios-xbox360-gpu-render-texture.elf
verify 9e16ecad7ae19049dfe535c333d5d3cc XenosRTT.mkext
verify 301c4fdb67d1c5055c1cbc4353d842df kboot-gpu-indexed-quad.conf
verify 115202953caf08bbf491f79ab153a2f0 openbios-xbox360-gpu-indexed-quad.elf
verify a9c3d97947e415f9c5b6591c57422f51 XenosIDX.mkext
verify 4139f3ef00dab9ff0a70a006fa9d1b90 kboot-gpu-alpha-blend.conf
verify 5b1bdb110a83f384eae679fe1a5b9925 openbios-xbox360-gpu-alpha-blend.elf
verify 29d4dc813afaafd780fe0eaf4d5a889f XenosALP.mkext
verify e097883a629e92c0458b9b236839b3c0 kboot-gpu-depth-triangles.conf
verify 642afc5ff603a5e1ab9be9be60fdb672 openbios-xbox360-gpu-depth-triangles.elf
verify 90d35bc89bdaf2eb87acaac24240d569 XenosZBF.mkext
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
for name in "$archive" "$elf" "$config" "$app"; do
    if test -e "$name"; then
        echo "$name already exists" >&2
        exit 1
    fi
done

scratch=$(mktemp -d /tmp/xenos-reusable-surface-stage.XXXXXXXX)
trap 'rm -rf "$scratch"' EXIT
boot_checksums > "$scratch/before"
/usr/bin/tar -xf - -C "$scratch"
verify "$archive_md5" "$scratch/$archive"
verify "$elf_md5" "$scratch/$elf"
verify "$config_md5" "$scratch/$config"
verify "$app_md5" "$scratch/$app"
chmod +x "$scratch/$app"
test "$("$scratch/$app" --version)" = 'XenosSurface 1'
exit_code=0
"$scratch/$app" --unexpected >"$scratch/usage" 2>&1 || exit_code=$?
test "$exit_code" -eq 2
grep -F 'Usage:' "$scratch/usage" >/dev/null
printf '%s\n' 'Verified the XenosSurface executable and input validation on Tiger.'
mkdir "$scratch/unpacked"
/usr/sbin/mkextunpack -v -a ppc -d "$scratch/unpacked" "$scratch/$archive" > "$scratch/bundles"
cat "$scratch/bundles"
test "$(find "$scratch/unpacked" -name '*.kext' -type d | wc -l | tr -d ' ')" = 9
grep -F 'XenonCoreGraphics - fish.goldfish64.osx360.XenonCoreGraphics (0.0.5d35)' "$scratch/bundles" >/dev/null
/sbin/kextload -n -t -z "$scratch/unpacked/XenonCoreGraphics.kext"

cp -n "$scratch/$archive" "$scratch/$elf" "$scratch/$config" "$scratch/$app" "$boot/"
sync
verify "$archive_md5" "$archive"
verify "$elf_md5" "$elf"
verify "$config_md5" "$config"
verify "$app_md5" "$app"
test "$("$boot/$app" --version)" = 'XenosSurface 1' 
boot_checksums > "$scratch/after"
cmp "$scratch/before" "$scratch/after"
printf 'Verified the staged reusable surface test and %s preserved boot files and backups.\n' "$(wc -l < "$scratch/after" | tr -d ' ')"
