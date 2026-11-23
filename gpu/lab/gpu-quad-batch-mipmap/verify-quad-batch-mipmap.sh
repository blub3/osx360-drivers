set -eu
boot=/Volumes/OSX360BOOT
if ! /usr/sbin/kextstat | grep -F 'XenonCoreGraphics (0.0.5d31)'; then
    printf '%s\n' 'The mipmap test needs a cold boot with XenonCoreGraphics 0.0.5d31.' >&2
    exit 2
fi
cd "$boot"
test "$(md5 -q kboot.conf)" = 6a2f4b93150dee38b6ffd4262d047dc9
test "$(md5 -q kboot.conf.before-gpuquadmipmap.bak)" = d5d0d3cd530e38cad535deb4a2706c82
test "$(md5 -q openbios-xbox360.720p-hdmi.elf.bak)" = 2ce8cf67dd57d0ef8b68beaa342e051a
test "$(md5 -q openbios-xbox360-native-hdmi-test.elf)" = ca72c036347285e3fe77b58d3b2c93c6
test "$(md5 -q XenosQuadBatchMipmap)" = e9a00ddd30f3240935db9bce8e1c544e

run_seed() {
    seed=$1
    printf '\nSeed %s\n' "$seed"
    if ! result=$(./XenosQuadBatchMipmap --seed "$seed" --depth-split 16 2>&1); then
        printf '%s\n' "$result"
        /sbin/dmesg | grep -F 'XenosBatchMipmap:' | tail -n 8
        return 1
    fi
    printf '%s\n' "$result"
    ticket=$(printf '%s\n' "$result" | sed -n 's/^PASS ticket=\([0-9][0-9]*\) state=1 fence=\1 pixels=3072 mismatches=0 .*$/\1/p')
    test -n "$ticket"
    logs=$(/sbin/dmesg | grep -F "XenosBatchMipmap: PASS ticket=$ticket " | tail -n 1)
    if test -z "$logs" && test -r /var/log/system.log; then
        logs=$(grep -F "XenosBatchMipmap: PASS ticket=$ticket " /var/log/system.log | tail -n 1)
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
                   value["sample_mismatches"] == "0" && value["level0_pixels"] == "144" &&
                   value["level1_pixels"] == "256" && value["guard_pixels"] == "624" &&
                   value["level1_evidence"] + 0 > 0 && value["level1_evidence"] + 0 <= 256 &&
                   length(value["mip_hash"]) == 8 && length(value["sample_hash"]) == 8)
        }
    '
}
run_seed 1
sleep 1
run_seed 67
