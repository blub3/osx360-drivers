set -eu
boot=/Volumes/OSX360BOOT
new_boot=$(/usr/sbin/sysctl -n kern.boottime)
test "$new_boot" -gt 1795336107
printf 'Cold boot timestamp %s\n' "$new_boot"
/usr/sbin/kextstat | grep -F 'XenonCoreGraphics (0.0.5d23)'
cd "$boot"
test "$(/sbin/md5 -q kboot.conf)" = 1d26b704eda4c232b35d199d76c7db35
test "$(/sbin/md5 -q XenosQuadBatchScissor)" = 0d947063ed567c09020545d98fab2526
test "$(/sbin/md5 -q kboot.conf.before-gpuquadscissor.bak)" = c092723515ab7afb2d7f8ae6ff9de0a2
test "$(/sbin/md5 -q openbios-xbox360.720p-hdmi.elf.bak)" = 2ce8cf67dd57d0ef8b68beaa342e051a
test "$(/sbin/md5 -q openbios-xbox360-native-hdmi-test.elf)" = ca72c036347285e3fe77b58d3b2c93c6

status=0
output=$(./XenosQuadBatchScissor --seed 1 2>&1) || status=$?
printf '%s\n' "$output"
logs=$(/sbin/dmesg | grep -F 'XenosBatchScissor:' | tail -n 8)
if test -z "$logs" && test -r /var/log/system.log; then
    logs=$(grep -F 'XenosBatchScissor:' /var/log/system.log | tail -n 8 | sed 's/^.*kernel\[[0-9]*\]: //')
fi
printf '\nDriver log\n%s\n' "$logs"
/usr/sbin/ioreg -c XenosController -w 0 | grep -E 'XenosBatchScissor|XenosUserQuad(Fence|Mismatches|PixelsChecked|Result|Source)' || true
test "$status" -eq 0
printf '%s\n%s\n' "$output" "$logs" | awk '
    /^Uploaded / {
        ticket = $2
        sub(/^ticket=/, "", ticket)
        submitted = ticket + 0 > 0 && $3 == "bytes=1830" && $4 == "draws=2" &&
                    $5 == "seed=1" && $6 == "rect=13,11,24,23" &&
                    $7 == "clipped_back=30" && $8 == "clipped_clear=198" && $9 == "front_only=92" &&
                    $10 == "overlap_alpha64=10" && $11 == "overlap_alpha192=30" &&
                    $12 == "reference_rgb_hash=317d1636"
    }
    /^PASS / {
        hash = $8
        sub(/^readback_rgb_hash=/, "", hash)
        passed = $2 == "ticket=" ticket && $3 == "state=1" && $4 == "fence=" ticket &&
                 $5 == "pixels=1024" && $6 == "mismatches=0" && $7 == "blend_tolerance=1" &&
                 hash ~ /^[0-9a-f]+$/ && length(hash) == 8
    }
    /^XenosBatchScissor: PASS / {
        hardware = $3 == "ticket=" ticket && $4 == "draws=2" && $5 == "fence=" ticket &&
                   $6 == "pixels=1024" && $7 == "mismatches=0" && $8 == "rect=13,11,24,23" &&
                   $9 == "clipped_back=30" && $10 == "clipped_clear=198" &&
                   $11 == "front_only=92" && $12 == "overlap_blended=40" &&
                   ($13 == "max_delta=0" || $13 == "max_delta=1") && $14 == "hash=" hash
    }
    END { exit !(submitted && passed && hardware) }
'
