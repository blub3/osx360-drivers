set -eu
boot=/Volumes/OSX360BOOT
new_boot=$(/usr/sbin/sysctl -n kern.boottime)
test "$new_boot" -gt "$1"
printf 'Cold boot timestamp %s\n' "$new_boot"
/usr/sbin/kextstat | grep -F 'XenonCoreGraphics (0.0.5d28)'
cd "$boot"
test "$(/sbin/md5 -q kboot.conf)" = 34bac7c3cb547e780d0c30bc589272b8
test "$(/sbin/md5 -q XenosQuadBatchRenderTexture)" = cda99cdbd7a7ee7c3a8c96d559623345
test "$(/sbin/md5 -q kboot.conf.before-gpuquadrtt.bak)" = fbfdaf0c08e94e6c9ae2d66503395ff2
test "$(/sbin/md5 -q openbios-xbox360.720p-hdmi.elf.bak)" = 2ce8cf67dd57d0ef8b68beaa342e051a
test "$(/sbin/md5 -q openbios-xbox360-native-hdmi-test.elf)" = ca72c036347285e3fe77b58d3b2c93c6

scratch=$(mktemp -d /tmp/xenos-quad-rtt.XXXXXXXX)
trap 'rm -rf "$scratch"' EXIT
./XenosQuadBatchRenderTexture --seed 1 --depth-split 16 2>&1 | tee "$scratch/result"
logs=$(/sbin/dmesg | grep -F 'XenosBatchRenderTexture:' | tail -n 8)
if test -z "$logs" && test -r /var/log/system.log; then
    logs=$(grep -F 'XenosBatchRenderTexture:' /var/log/system.log | tail -n 8 | sed 's/^.*kernel\[[0-9]*\]: //')
fi
printf '\nDriver log\n%s\n' "$logs"
/usr/sbin/ioreg -c XenosController -w 0 | grep -E 'XenosBatchRenderTexture|XenosUserQuad(Fence|Mismatches|PixelsChecked|Result|Source)' || true
{ cat "$scratch/result"; printf '%s\n' "$logs"; } | awk '
    /^Uploaded / {
        ticket = $2
        sub(/^ticket=/, "", ticket)
        submitted = ticket + 0 > 0 && $3 == "bytes=1839" && $4 == "passes=2" &&
                    $5 == "batch_draws=2" && $6 == "stencil_draws=2" && $7 == "seed=1" &&
                    $8 == "split_x=16" && $9 == "scissor_reject_only=37" &&
                    $10 == "stencil_reject_only=35" && $11 == "both_reject=253" &&
                    $12 == "depth_reject=10" && $13 == "front_only=18" && $14 == "overlap_alpha64=1" &&
                    $15 == "overlap_alpha192=6" && $16 == "first_reference_rgb_hash=5d06d64b"
    }
    /^PASS / {
        hash = $8
        sub(/^first_readback_rgb_hash=/, "", hash)
        passed = $2 == "ticket=" ticket && $3 == "state=1" && $4 == "fence=" ticket &&
                 $5 == "pixels=2048" && $6 == "mismatches=0" && $7 == "blend_tolerance=1" &&
                 hash ~ /^[0-9a-f]+$/ && length(hash) == 8
    }
    /^XenosBatchRenderTexture: PASS / {
        if ($3 == "ticket=" ticket) {
            sampled_hash = $15
            sub(/^sample_hash=/, "", sampled_hash)
            hardware = $4 == "fence=" ticket && $5 == "first_pixels=1024" &&
                       $6 == "first_mismatches=0" && $7 == "sample_pixels=1024" &&
                       $8 == "sample_mismatches=0" && $9 == "sampled_texels=900" &&
                       $10 == "guard_pixels=124" && $11 == "depth_reject=10" &&
                       $12 == "overlap_blended=7" && ($13 == "max_delta=0" || $13 == "max_delta=1") &&
                       $14 == "first_hash=" hash && $15 ~ /^sample_hash=[0-9a-f]+$/ &&
                       length(sampled_hash) == 8
        }
    }
    END { exit !(submitted && passed && hardware) }
'
