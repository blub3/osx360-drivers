set -eu
boot=/Volumes/OSX360BOOT
new_boot=$(/usr/sbin/sysctl -n kern.boottime)
test "$new_boot" -gt 1795335487
printf 'Cold boot timestamp %s\n' "$new_boot"
/usr/sbin/kextstat | grep -F 'XenonCoreGraphics (0.0.5d22)'
cd "$boot"
test "$(/sbin/md5 -q kboot.conf)" = c092723515ab7afb2d7f8ae6ff9de0a2
test "$(/sbin/md5 -q XenosQuadBatchBlend)" = 613a22cd6d22271fce8f5a4178668d0b
test "$(/sbin/md5 -q kboot.conf.before-gpuquadblend.bak)" = 26ef82029e50eb22c73f11bf87600cd0
test "$(/sbin/md5 -q openbios-xbox360.720p-hdmi.elf.bak)" = 2ce8cf67dd57d0ef8b68beaa342e051a
test "$(/sbin/md5 -q openbios-xbox360-native-hdmi-test.elf)" = ca72c036347285e3fe77b58d3b2c93c6

status=0
output=$(./XenosQuadBatchBlend --seed 1 2>&1) || status=$?
printf '%s\n' "$output"
logs=$(/sbin/dmesg | grep -F 'XenosBatchBlend:' | tail -n 8)
if test -z "$logs" && test -r /var/log/system.log; then
    logs=$(grep -F 'XenosBatchBlend:' /var/log/system.log | tail -n 8 | sed 's/^.*kernel\[[0-9]*\]: //')
fi
printf '\nDriver log\n%s\n' "$logs"
/usr/sbin/ioreg -c XenosController -w 0 | grep -E 'XenosBatchBlend|XenosUserQuad(Fence|Mismatches|PixelsChecked|Result|Source)' || true
test "$status" -eq 0
printf '%s\n%s\n' "$output" "$logs" | awk '
    /^Uploaded / {
        ticket = $2
        sub(/^ticket=/, "", ticket)
        submitted = ticket + 0 > 0 && $3 == "bytes=1826" && $4 == "draws=2" &&
                    $5 == "seed=1" && $6 == "back_only=186" && $7 == "front_only=290" &&
                    $8 == "overlap_alpha64=28" && $9 == "overlap_alpha192=42" &&
                    $10 == "reference_rgb_hash=aeafed8f"
    }
    /^PASS / {
        hash = $8
        sub(/^readback_rgb_hash=/, "", hash)
        passed = $2 == "ticket=" ticket && $3 == "state=1" && $4 == "fence=" ticket &&
                 $5 == "pixels=1024" && $6 == "mismatches=0" && $7 == "blend_tolerance=1" &&
                 hash ~ /^[0-9a-f]+$/ && length(hash) == 8
    }
    /^XenosBatchBlend: PASS / {
        hardware = $3 == "ticket=" ticket && $4 == "draws=2" && $5 == "fence=" ticket &&
                   $6 == "pixels=1024" && $7 == "mismatches=0" &&
                   $8 == "back_only=186" && $9 == "front_only=290" && $10 == "overlap_blended=70" &&
                   ($11 == "max_delta=0" || $11 == "max_delta=1") && $12 == "hash=" hash
    }
    END { exit !(submitted && passed && hardware) }
'
