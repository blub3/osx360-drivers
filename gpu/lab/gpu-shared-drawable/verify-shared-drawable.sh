set -eu
cd /Volumes/OSX360BOOT
/usr/sbin/kextstat | grep -F 'XenonCoreGraphics (0.0.5d36)'
test "$(md5 -q kboot.conf)" = 8c2c7324d86de82840dede3d238135e8
test "$(md5 -q kboot.conf.before-gpushared.bak)" = 15030074fc8a27f9c29e9d9b629e65be
test "$(md5 -q openbios-xbox360.720p-hdmi.elf.bak)" = 2ce8cf67dd57d0ef8b68beaa342e051a
test "$(md5 -q openbios-xbox360-native-hdmi-test.elf)" = ca72c036347285e3fe77b58d3b2c93c6
test "$(md5 -q XenosSharedDrawable)" = f4a4baf35a7c4e837586e1eb7093b407
result=0
output=$(./XenosSharedDrawable 2>&1) || result=$?
printf '%s\n' "$output"
logs=$(/sbin/dmesg | grep -F 'XenosSurface:' | tail -n 8)
if test -z "$logs" && test -r /var/log/system.log; then
    logs=$(grep -F 'XenosSurface:' /var/log/system.log | tail -n 8)
fi
printf '%s\n' "$logs"
test "$result" -eq 0
printf '%s\n%s\n' "$output" "$logs" | awk '
    /^PASS owner / {
        split($5, ticket, "=")
        if ($6 != "fence=" ticket[2] || $8 != "pixels=1024")
            invalid = 1
        if (!owner)
            owner = $3
        if (owner != $3 || ticket[2] in hash)
            invalid = 1
        hash[ticket[2]] = $9
        expected[ticket[2]] = "XenosSurface: PASS " $5 " " $6 " pixels=1024 mismatches=0 " $7 " " $9
        owners++
    }
    /^PASS reader / {
        split($4, ticket, "=")
        if (!reader)
            reader = $3
        if (reader != $3 || reader == owner || $5 != "pixels=1024" ||
            $6 != "mismatches=0" || $7 != hash[ticket[2]] || ticket[2] in seen)
            invalid = 1
        seen[ticket[2]] = 1
        readers++
    }
    /^PASS shared_drawable processes=2 clears=2 gpu_pixels=2048 reader_pixels=2048 / {
        shared = $0 ~ /mapped_bytes=4096 read_only=1 wrong_token_rejected=1 replay_rejected=1$/
    }
    /XenosSurface: PASS ticket=/ {
        logLine = substr($0, index($0, "XenosSurface:"))
        logs[logLine] = 1
    }
    END {
        for (id in expected)
            if (expected[id] in logs)
                fences++
        exit !(!invalid && owners == 2 && readers == 2 && fences == 2 && shared)
    }
'
