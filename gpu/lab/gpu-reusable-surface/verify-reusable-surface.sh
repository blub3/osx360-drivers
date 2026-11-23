set -eu
boot=/Volumes/OSX360BOOT
if ! /usr/sbin/kextstat | grep -F 'XenonCoreGraphics (0.0.5d35)'; then
    printf '%s\n' 'The surface test needs a cold boot with XenonCoreGraphics 0.0.5d35.' >&2
    exit 2
fi
cd "$boot"
test "$(md5 -q kboot.conf)" = 15030074fc8a27f9c29e9d9b629e65be
test "$(md5 -q kboot.conf.before-gpusurface.bak)" = ce1dbc90d3be77da13697d0644c3ca1f
test "$(md5 -q openbios-xbox360.720p-hdmi.elf.bak)" = 2ce8cf67dd57d0ef8b68beaa342e051a
test "$(md5 -q openbios-xbox360-native-hdmi-test.elf)" = ca72c036347285e3fe77b58d3b2c93c6
test "$(md5 -q XenosSurface)" = 053206b54a6afabb97351b25f52db96c
result=0
output=$(./XenosSurface 2>&1) || result=$?
printf '%s\n' "$output"
logs=$(/sbin/dmesg | grep -F 'XenosSurface:' | tail -n 8)
if test -z "$logs" && test -r /var/log/system.log; then
    logs=$(grep -F 'XenosSurface:' /var/log/system.log | tail -n 8)
fi
printf '%s\n' "$logs"
test "$result" -eq 0
printf '%s\n%s\n' "$output" "$logs" | awk '
    /^PASS handle=/ {
        if (split($2, handle, "=") != 2 || split($3, ticket, "=") != 2)
            exit 1
        if ($4 != "fence=" ticket[2] || $6 != "pixels=1024" || $7 != "mismatches=0")
            exit 1
        order[++runs] = handle[2]
        driverLog[ticket[2]] = "XenosSurface: PASS " $3 " " $4 " pixels=1024 mismatches=0 " $5 " " $8
    }
    /^PASS lifecycle clears=3 pixels=3072 / {
        lifecycle = $0 ~ /foreign_handles_rejected=3 stale_handles_rejected=3 stale_tickets_rejected=1$/
    }
    /XenosSurface: PASS ticket=/ {
        logLine = substr($0, index($0, "XenosSurface:"))
        logs[logLine] = 1
    }
    END {
        for (ticketNumber in driverLog)
            if (driverLog[ticketNumber] in logs)
                matchingFences++
        exit !(runs == 3 && matchingFences == 3 && lifecycle && order[1] == order[2] && order[1] != order[3])
    }
'
