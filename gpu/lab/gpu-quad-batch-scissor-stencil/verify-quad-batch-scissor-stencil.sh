set -eu
boot=/Volumes/OSX360BOOT
new_boot=$(/usr/sbin/sysctl -n kern.boottime)
test "$new_boot" -gt "$1"
printf 'Cold boot timestamp %s\n' "$new_boot"
/usr/sbin/kextstat | grep -F 'XenonCoreGraphics (0.0.5d26)'
cd "$boot"
test "$(/sbin/md5 -q kboot.conf)" = 11696b2c911eb71b3e692c356ed3766c
test "$(/sbin/md5 -q XenosQuadBatchScissorStencil)" = 88c5c86fdc00f3e6b2606bd7777cb719
test "$(/sbin/md5 -q kboot.conf.before-gpuquadclip.bak)" = 6f72a8cbc2f89194edd83db7f484f736
test "$(/sbin/md5 -q openbios-xbox360.720p-hdmi.elf.bak)" = 2ce8cf67dd57d0ef8b68beaa342e051a
test "$(/sbin/md5 -q openbios-xbox360-native-hdmi-test.elf)" = ca72c036347285e3fe77b58d3b2c93c6

scratch=$(mktemp -d /tmp/xenos-stencil-retry.XXXXXXXX)
trap 'rm -rf "$scratch"' EXIT
./XenosQuadBatchScissorStencil --seed 1 2>&1 | tee "$scratch/result"
output=$(cat "$scratch/result")
logs=$(/sbin/dmesg | grep -F 'XenosBatchScissorStencil:' | tail -n 8)
if test -z "$logs" && test -r /var/log/system.log; then
    logs=$(grep -F 'XenosBatchScissorStencil:' /var/log/system.log | tail -n 8 | sed 's/^.*kernel\[[0-9]*\]: //')
fi
printf '\nDriver log\n%s\n' "$logs"
/usr/sbin/ioreg -c XenosController -w 0 | grep -E 'XenosBatchScissorStencil|XenosUserQuad(Fence|Mismatches|PixelsChecked|Result|Source)' || true
printf '%s\n%s\n' "$output" "$logs" | awk '
    /^Uploaded / {
        ticket = $2
        sub(/^ticket=/, "", ticket)
        submitted = ticket + 0 > 0 && $3 == "bytes=1838" && $4 == "draws=2" &&
                    $5 == "stencil_draws=2" && $6 == "seed=1" &&
                    $7 == "scissor_reject_only=37" && $8 == "stencil_reject_only=35" &&
                    $9 == "both_reject=253" && $10 == "front_only=18" &&
                    $11 == "overlap_alpha64=3" && $12 == "overlap_alpha192=14" &&
                    $13 == "reference_rgb_hash=c5844b2f"
    }
    /^PASS / {
        hash = $8
        sub(/^readback_rgb_hash=/, "", hash)
        passed = $2 == "ticket=" ticket && $3 == "state=1" && $4 == "fence=" ticket &&
                 $5 == "pixels=1024" && $6 == "mismatches=0" && $7 == "blend_tolerance=1" &&
                 hash ~ /^[0-9a-f]+$/ && length(hash) == 8
    }
    /^XenosBatchScissorStencil: PASS / {
        if ($3 == "ticket=" ticket) {
            hardware = $4 == "draws=2" && $5 == "stencil_draws=2" && $6 == "fence=" ticket &&
                       $7 == "pixels=1024" && $8 == "mismatches=0" &&
                       $9 == "scissor_reject_only=37" && $10 == "stencil_reject_only=35" &&
                       $11 == "both_reject=253" && $12 == "front_only=18" && $13 == "overlap_blended=17" &&
                       ($14 == "max_delta=0" || $14 == "max_delta=1") && $15 == "hash=" hash
        }
    }
    END { exit !(submitted && passed && hardware) }
'
