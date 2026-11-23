set -eu
boot=/Volumes/OSX360BOOT
if ! /usr/sbin/kextstat | grep -F 'XenonCoreGraphics (0.0.5d34)'; then
    printf '%s\n' 'The anisotropic test needs a cold boot with XenonCoreGraphics 0.0.5d34.' >&2
    exit 2
fi
cd "$boot"
cmp kboot.conf kboot-gpu-quad-batch-anisotropic.conf
test "$(md5 -q kboot.conf.before-gpuquadaniso.bak)" = 40dcf17a835dc47ec21dc92d7d79fa81
test "$(md5 -q openbios-xbox360.720p-hdmi.elf.bak)" = 2ce8cf67dd57d0ef8b68beaa342e051a
test "$(md5 -q openbios-xbox360-native-hdmi-test.elf)" = ca72c036347285e3fe77b58d3b2c93c6
test "$(./XenosQuadBatchAnisotropic --version)" = "XenosQuadBatchAnisotropic 1"

run_seed() {
    seed=$1
    printf '\nSeed %s\n' "$seed"
    if ! result=$(./XenosQuadBatchAnisotropic --seed "$seed" --depth-split 16 2>&1); then
        printf '%s\n' "$result"
        /sbin/dmesg | grep -F 'XenosBatchAnisotropic:' | tail -n 8
        return 1
    fi
    printf '%s\n' "$result"
    ticket=$(printf '%s\n' "$result" | sed -n 's/^PASS ticket=\([0-9][0-9]*\) state=1 fence=\1 pixels=2048 mismatches=0 .*$/\1/p')
    test -n "$ticket"
    logs=$(/sbin/dmesg | grep -F "XenosBatchAnisotropic: PASS ticket=$ticket " | tail -n 1)
    if test -z "$logs" && test -r /var/log/system.log; then
        logs=$(grep -F "XenosBatchAnisotropic: PASS ticket=$ticket " /var/log/system.log | tail -n 1)
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
                   value["sample_pixels"] == "1024" && value["first_mismatches"] == "0" &&
                   value["sample_mismatches"] == "0" && value["control_pixels"] == "72" &&
                   value["dark_pixels"] == "36" && value["light_pixels"] == "36" &&
                   value["guard_pixels"] == "880" && value["dark_max"] + 0 <= 96 &&
                   value["light_min"] + 0 >= 160 &&
                   length(value["first_hash"]) == 8 && length(value["sample_hash"]) == 8)
        }
    '
}
run_seed 67
