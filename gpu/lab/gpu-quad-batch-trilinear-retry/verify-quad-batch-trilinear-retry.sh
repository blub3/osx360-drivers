set -eu
boot=/Volumes/OSX360BOOT
if ! /usr/sbin/kextstat | grep -F 'XenonCoreGraphics (0.0.5d33)'; then
    printf '%s\n' 'The trilinear retry needs a cold boot with XenonCoreGraphics 0.0.5d33.' >&2
    exit 2
fi
cd "$boot"
test "$(md5 -q kboot.conf)" = 40dcf17a835dc47ec21dc92d7d79fa81
test "$(md5 -q kboot.conf.before-gpuquadtriretry.bak)" = aee74a223e91c787a95b6d398fab8285
test "$(md5 -q openbios-xbox360.720p-hdmi.elf.bak)" = 2ce8cf67dd57d0ef8b68beaa342e051a
test "$(md5 -q openbios-xbox360-native-hdmi-test.elf)" = ca72c036347285e3fe77b58d3b2c93c6
test "$(md5 -q XenosQuadBatchTrilinearRetry)" = 716f2efe129960f4701e31ac57d6416e

run_seed() {
    seed=$1
    printf '\nSeed %s\n' "$seed"
    if ! result=$(./XenosQuadBatchTrilinearRetry --seed "$seed" --depth-split 16 2>&1); then
        printf '%s\n' "$result"
        /sbin/dmesg | grep -F 'XenosBatchTrilinear:' | tail -n 8
        return 1
    fi
    printf '%s\n' "$result"
    ticket=$(printf '%s\n' "$result" | sed -n 's/^PASS ticket=\([0-9][0-9]*\) state=1 fence=\1 pixels=3072 mismatches=0 .*$/\1/p')
    test -n "$ticket"
    logs=$(/sbin/dmesg | grep -F "XenosBatchTrilinear: PASS ticket=$ticket " | tail -n 1)
    if test -z "$logs" && test -r /var/log/system.log; then
        logs=$(grep -F "XenosBatchTrilinear: PASS ticket=$ticket " /var/log/system.log | tail -n 1)
    fi
    printf '%s\n' "$logs"
    printf '%s\n' "$logs" | awk -v ticket="$ticket" '
        {
            for (i = 1; i <= NF; ++i) {
                if (split($i, field, "=") == 2)
                    value[field[1]] = field[2]
            }
        }
        END {
            exit !(value["fence"] == ticket && value["first_pixels"] == "1024" &&
                   value["mip_pixels"] == "1024" && value["sample_pixels"] == "1024" &&
                   value["first_mismatches"] == "0" && value["mip_mismatches"] == "0" &&
                   value["sample_mismatches"] == "0" && value["level0_pixels"] == "128" &&
                   value["level1_pixels"] == "128" && value["guard_pixels"] == "640" &&
                   value["blended_pixels"] == "128" && value["max_delta"] ~ /^[0-5]$/ &&
                   value["blend_evidence"] + 0 > 0 && value["blend_evidence"] + 0 <= 128 &&
                   length(value["mip_hash"]) == 8 && length(value["sample_hash"]) == 8)
        }
    '
}
run_seed 67
