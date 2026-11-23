set -eu
boot=/Volumes/OSX360BOOT
new_boot=$(/usr/sbin/sysctl -n kern.boottime)
test "$new_boot" -gt "$1"
printf 'Cold boot timestamp %s\n' "$new_boot"
/usr/sbin/kextstat | grep -F 'XenonCoreGraphics (0.0.5d30)'
cd "$boot"
test "$(/sbin/md5 -q kboot.conf)" = d5d0d3cd530e38cad535deb4a2706c82
test "$(/sbin/md5 -q XenosQuadBatchBilinearRetry)" = 4a84a1585ea506ed761aaf46011056cb
test "$(/sbin/md5 -q kboot.conf.before-gpuquadbilinear.bak)" = 34bac7c3cb547e780d0c30bc589272b8
test "$(/sbin/md5 -q kboot.conf.before-bilinear-retry.bak)" = a19a6f81b4e28111569c3e90783d0cf2
test "$(/sbin/md5 -q openbios-xbox360.720p-hdmi.elf.bak)" = 2ce8cf67dd57d0ef8b68beaa342e051a
test "$(/sbin/md5 -q openbios-xbox360-native-hdmi-test.elf)" = ca72c036347285e3fe77b58d3b2c93c6

scratch=$(mktemp -d /tmp/xenos-quad-bilinear.XXXXXXXX)
trap 'rm -rf "$scratch"' EXIT
run_seed() {
    seed=$1
    first_ref=$2
    expected_sample_hash=$3
    printf '\nSeed %s\n' "$seed"
./XenosQuadBatchBilinearRetry --seed "$seed" --depth-split 16 2>&1 | tee "$scratch/result"
logs=$(/sbin/dmesg | grep -F 'XenosBatchBilinear:' | tail -n 8)
if test -z "$logs" && test -r /var/log/system.log; then
    logs=$(grep -F 'XenosBatchBilinear:' /var/log/system.log | tail -n 8 | sed 's/^.*kernel\[[0-9]*\]: //')
fi
printf '\nDriver log\n%s\n' "$logs"
/usr/sbin/ioreg -c XenosController -w 0 | grep -E  'XenosBatchBilinear|XenosBatchRenderTexture|XenosUserQuad(Fence|Mismatches|PixelsChecked|Result|Source)' || true
{ cat "$scratch/result"; printf '%s\n' "$logs"; } | awk -v seed="$seed" -v first_ref="$first_ref" -v expected_sample_hash="$expected_sample_hash" '
    /^Uploaded / {
        ticket = $2
        sub(/^ticket=/, "", ticket)
        submitted = ticket + 0 > 0 && $3 == "bytes=1839" && $4 == "passes=2" &&
                    $5 == "batch_draws=2" && $6 == "stencil_draws=2" && $7 == "seed=" seed &&
                    $8 == "split_x=16" && $9 == "scissor_reject_only=37" &&
                    $10 == "stencil_reject_only=35" && $11 == "both_reject=253" &&
                    $12 == "depth_reject=10" && $13 == "front_only=18" && $14 == "overlap_alpha64=1" &&
                    $15 == "overlap_alpha192=6" && $16 == "first_reference_rgb_hash=" first_ref
    }
    /^PASS / {
        hash = $9
        sub(/^first_readback_rgb_hash=/, "", hash)
        passed = $2 == "ticket=" ticket && $3 == "state=1" && $4 == "fence=" ticket &&
                 $5 == "pixels=2048" && $6 == "mismatches=0" && $7 == "blend_tolerance=1" &&
                 $8 == "filter_tolerance=3" &&
                 hash ~ /^[0-9a-f]+$/ && length(hash) == 8
    }
    /^XenosBatchBilinear: PASS / {
        if ($3 == "ticket=" ticket) {
            sampled_hash = $14
            sub(/^sample_hash=/, "", sampled_hash)
            witnesses = $11
            sub(/^bilinear_witnesses=/, "", witnesses)
            hardware = $4 == "fence=" ticket && $5 == "first_pixels=1024" &&
                       $6 == "first_mismatches=0" && $7 == "sample_pixels=1024" &&
                       $8 == "sample_mismatches=0" && $9 == "filtered_pixels=900" &&
                       $10 == "guard_pixels=124" && $11 ~ /^bilinear_witnesses=[0-9]+$/ &&
                       witnesses + 0 > 0 && witnesses + 0 <= 900 && $12 ~ /^filter_max_delta=[0-3]$/ &&
                       $13 == "first_hash=" hash && $14 ~ /^sample_hash=[0-9a-f]+$/ &&
                       sampled_hash == expected_sample_hash
        }
    }
    END { exit !(submitted && passed && hardware) }
'
}
run_seed 1 5d06d64b f6221a06
sleep 1
run_seed 67 b86650b0 e9183cc8
