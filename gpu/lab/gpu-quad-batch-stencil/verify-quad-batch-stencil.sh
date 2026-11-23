set -eu
boot=/Volumes/OSX360BOOT
new_boot=$(/usr/sbin/sysctl -n kern.boottime)
test "$new_boot" -gt 1795336708
printf 'Cold boot timestamp %s\n' "$new_boot"
/usr/sbin/kextstat | grep -F 'XenonCoreGraphics (0.0.5d24)'
cd "$boot"
test "$(/sbin/md5 -q kboot.conf)" = 48556223bb33f3b4a2a4e678e00c3714
test "$(/sbin/md5 -q XenosQuadBatchStencil)" = 097457470d3c71b522604a6a363ec4c4
test "$(/sbin/md5 -q kboot.conf.before-gpuquadstencil.bak)" = 1d26b704eda4c232b35d199d76c7db35
test "$(/sbin/md5 -q openbios-xbox360.720p-hdmi.elf.bak)" = 2ce8cf67dd57d0ef8b68beaa342e051a
test "$(/sbin/md5 -q openbios-xbox360-native-hdmi-test.elf)" = ca72c036347285e3fe77b58d3b2c93c6

status=0
output=$(./XenosQuadBatchStencil --seed 1 2>&1) || status=$?
printf '%s\n' "$output"
logs=$(/sbin/dmesg | grep -F 'XenosBatchStencil:' | tail -n 8)
if test -z "$logs" && test -r /var/log/system.log; then
    logs=$(grep -F 'XenosBatchStencil:' /var/log/system.log | tail -n 8 | sed 's/^.*kernel\[[0-9]*\]: //')
fi
printf '\nDriver log\n%s\n' "$logs"
/usr/sbin/ioreg -c XenosController -w 0 | grep -E 'XenosBatchStencil|XenosUserQuad(Fence|Mismatches|PixelsChecked|Result|Source)' || true
test "$status" -eq 0
printf '%s\n%s\n' "$output" "$logs" | awk '
    /^Uploaded / {
        ticket = $2
        sub(/^ticket=/, "", ticket)
        submitted = ticket + 0 > 0 && $3 == "bytes=1834" && $4 == "draws=2" &&
                    $5 == "stencil_draws=2" && $6 == "seed=1" &&
                    $7 == "masked_back=44" && $8 == "masked_clear=244" && $9 == "front_only=46" &&
                    $10 == "overlap_alpha64=8" && $11 == "overlap_alpha192=18" &&
                    $12 == "reference_rgb_hash=eaecd844"
    }
    /^PASS / {
        hash = $8
        sub(/^readback_rgb_hash=/, "", hash)
        passed = $2 == "ticket=" ticket && $3 == "state=1" && $4 == "fence=" ticket &&
                 $5 == "pixels=1024" && $6 == "mismatches=0" && $7 == "blend_tolerance=1" &&
                 hash ~ /^[0-9a-f]+$/ && length(hash) == 8
    }
    /^XenosBatchStencil: PASS / {
        if ($3 == "ticket=" ticket) {
            hardware = $4 == "draws=2" && $5 == "stencil_draws=2" && $6 == "fence=" ticket &&
                       $7 == "pixels=1024" && $8 == "mismatches=0" &&
                       $9 == "masked_back=44" && $10 == "masked_clear=244" &&
                       $11 == "front_only=46" && $12 == "overlap_blended=26" &&
                       ($13 == "max_delta=0" || $13 == "max_delta=1") && $14 == "hash=" hash
        }
    }
    END { exit !(submitted && passed && hardware) }
'
