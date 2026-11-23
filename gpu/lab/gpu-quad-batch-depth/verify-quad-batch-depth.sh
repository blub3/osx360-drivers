set -eu
boot=/Volumes/OSX360BOOT
new_boot=$(/usr/sbin/sysctl -n kern.boottime)
test "$new_boot" -gt "$1"
printf 'Cold boot timestamp %s\n' "$new_boot"
/usr/sbin/kextstat | grep -F 'XenonCoreGraphics (0.0.5d27)'
cd "$boot"
test "$(/sbin/md5 -q kboot.conf)" = fbfdaf0c08e94e6c9ae2d66503395ff2
test "$(/sbin/md5 -q XenosQuadBatchDepth)" = 6c69d92a88e232ec22826b9969d24ee7
test "$(/sbin/md5 -q kboot.conf.before-gpuquaddepth.bak)" = 11696b2c911eb71b3e692c356ed3766c
test "$(/sbin/md5 -q openbios-xbox360.720p-hdmi.elf.bak)" = 2ce8cf67dd57d0ef8b68beaa342e051a
test "$(/sbin/md5 -q openbios-xbox360-native-hdmi-test.elf)" = ca72c036347285e3fe77b58d3b2c93c6

scratch=$(mktemp -d /tmp/xenos-quad-depth.XXXXXXXX)
trap 'rm -rf "$scratch"' EXIT
run_depth() {
    split=$1
    rejects=$2
    low=$3
    high=$4
    ref=$5
    printf '\nDepth split at X=%s\n' "$split"
    ./XenosQuadBatchDepth --seed 1 --depth-split "$split" 2>&1 | tee "$scratch/result"
    logs=$(/sbin/dmesg | grep -F 'XenosBatchDepth:' | tail -n 8)
    if test -z "$logs" && test -r /var/log/system.log; then
        logs=$(grep -F 'XenosBatchDepth:' /var/log/system.log | tail -n 8 | sed 's/^.*kernel\[[0-9]*\]: //')
    fi
    printf '%s\n' "$logs" | tail -n 2
    { cat "$scratch/result"; printf '%s\n' "$logs"; } | awk -v split_x="$split"         -v rejects="$rejects" -v low="$low" -v high="$high" -v ref="$ref" '
        /^Uploaded / {
            ticket = $2
            sub(/^ticket=/, "", ticket)
            submitted = ticket + 0 > 0 && $3 == "bytes=1839" && $4 == "draws=2" &&
                        $5 == "stencil_draws=2" && $6 == "seed=1" && $7 == "split_x=" split_x &&
                        $8 == "scissor_reject_only=37" && $9 == "stencil_reject_only=35" &&
                        $10 == "both_reject=253" && $11 == "depth_reject=" rejects && $12 == "front_only=18" &&
                        $13 == "overlap_alpha64=" low && $14 == "overlap_alpha192=" high &&
                        $15 == "reference_rgb_hash=" ref
        }
        /^PASS / {
            hash = $8
            sub(/^readback_rgb_hash=/, "", hash)
            passed = $2 == "ticket=" ticket && $3 == "state=1" && $4 == "fence=" ticket &&
                     $5 == "pixels=1024" && $6 == "mismatches=0" && $7 == "blend_tolerance=1" &&
                     hash ~ /^[0-9a-f]+$/ && length(hash) == 8
        }
        /^XenosBatchDepth: PASS / {
            if ($3 == "ticket=" ticket) {
                hardware = $4 == "draws=2" && $5 == "stencil_draws=2" && $6 == "fence=" ticket &&
                           $7 == "pixels=1024" && $8 == "mismatches=0" && $9 == "split_x=" split_x &&
                           $10 == "scissor_reject_only=37" && $11 == "stencil_reject_only=35" &&
                           $12 == "both_reject=253" && $13 == "depth_reject=" rejects &&
                           $14 == "front_only=18" && $15 == "overlap_blended=" (low + high) &&
                           ($16 == "max_delta=0" || $16 == "max_delta=1") && $17 == "hash=" hash
            }
        }
        END { exit !(submitted && passed && hardware) }
    '
}
run_depth 16 10 1 6 b4102ccb
sleep 1
run_depth 0 0 3 14 c5844b2f
sleep 1
run_depth 32 17 0 0 770c0eb0
