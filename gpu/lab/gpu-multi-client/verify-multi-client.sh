set -eu
cd /Volumes/OSX360BOOT
/usr/sbin/kextstat | grep -F 'XenonCoreGraphics (0.0.5d37)'
test "$(md5 -q kboot.conf)" = 2da466ec6ff69691f4c8cfa3ce29cf3f
test "$(md5 -q kboot.conf.before-gpumulticlient.bak)" = 8c2c7324d86de82840dede3d238135e8
test "$(md5 -q openbios-xbox360.720p-hdmi.elf.bak)" = 2ce8cf67dd57d0ef8b68beaa342e051a
test "$(md5 -q openbios-xbox360-native-hdmi-test.elf)" = ca72c036347285e3fe77b58d3b2c93c6
test "$(md5 -q XenosMultiClient)" = d0c2e5a02f30777215a7cc3b65951719
result=0
output=$(./XenosMultiClient 2>&1) || result=$?
printf '%s\n' "$output"
logs=$(/sbin/dmesg | grep -E 'XenosSurface(Queue)?:' | tail -n 40)
if test -z "$logs" && test -r /var/log/system.log; then
    logs=$(grep -E 'XenosSurface(Queue)?:' /var/log/system.log | tail -n 40)
fi
printf '%s\n' "$logs"
test "$result" -eq 0
printf '%s\n%s\n' "$output" "$logs" | awk '
    /^PASS owner / {
        split($5, ticket, "=")
        if ($6 != "fence=" ticket[2] || $8 != "pixels=1024" || ticket[2] in expected)
            invalid = 1
        if (!($3 in processes))
            clients++
        processes[$3] = 1
        expected[ticket[2]] = "XenosSurface: PASS " $5 " " $6 " pixels=1024 mismatches=0 " $7 " " $9
        clears++
    }
    /^PASS fifo / { fifo = 1 }
    /^PASS exit / { exited = $0 ~ /pending=0 active=0$/ }
    /^PASS buffers / {
        split($3, initial, "=")
        split($4, finalCount, "=")
        buffers = initial[2] == finalCount[2]
    }
    /^PASS multi_client processes=2 verified_clears=4 pixels=4096 fifo=1 exit_cleanup=1$/ { total = 1 }
    /XenosSurface: PASS ticket=/ {
        logLine = substr($0, index($0, "XenosSurface:"))
        logs[logLine] = 1
    }
    END {
        for (id in expected)
            if (expected[id] in logs)
                fences++
        exit !(!invalid && clears == 4 && clients == 2 && fences == 4 && fifo && exited && buffers && total)
    }
'
