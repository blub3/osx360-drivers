set -eu
previous_boot=1795334860
boot=/Volumes/OSX360BOOT
new_boot=$(/usr/sbin/sysctl -n kern.boottime)
test "$new_boot" -gt "$previous_boot"
printf 'Cold boot timestamp %s\n' "$new_boot"
/usr/sbin/kextstat | grep -F 'XenonCoreGraphics (0.0.5d21)'
cd "$boot"
test "$(/sbin/md5 -q kboot.conf)" = 26ef82029e50eb22c73f11bf87600cd0
test "$(/sbin/md5 -q XenosQuadBatch)" = 6a9a9452712ef68318722b9092caad19

run_case() {
    expected_hash=$1
    expected_first=$2
    expected_second=$3
    shift 3
    output=$(./XenosQuadBatch --mode overlap --seed 1 "$@" 2>&1) || {
        printf '%s\n' "$output"
        return 1
    }
    printf '%s\n' "$output"
    printf '%s\n' "$output" | awk -v hash="$expected_hash"         -v first="$expected_first" -v second="$expected_second" '
        /^Uploaded / {
            ticket = $2
            sub(/^ticket=/, "", ticket)
            submitted = (ticket + 0 > 0 && $3 == "bytes=1570" && $4 == "draws=2" &&
                         $8 == "first_only=" first && $9 == "second_only=" second &&
                         $10 == "overlap=70" && $11 == "expected_hash=" hash)
        }
        /^PASS / {
            passed = ($2 == "ticket=" ticket && $3 == "state=1" && $4 == "fence=" ticket &&
                      $5 == "pixels=1024" && $6 == "mismatches=0" &&
                      $7 == "expected_hash=" hash && $8 == "gpu_hash=" hash)
        }
        END { exit !(submitted && passed) }
    '
}
result=0
printf '\nNormal draw order\n'
run_case 95dd9f7d 186 290 || result=1
sleep 2
printf '\nReversed draw order\n'
run_case 325652f2 290 186 --reverse || result=1
printf '\nDriver log\n'
if ! /sbin/dmesg | grep -F 'XenosBatchQuad:' | tail -n 8; then
    true
fi
if test -r /var/log/system.log; then
    grep -F 'XenosBatchQuad:' /var/log/system.log | tail -n 8
fi
/usr/sbin/ioreg -c XenosController -w 0 | grep -E 'XenosBatchQuad|XenosUserQuad(Fence|Mismatches|PixelsChecked)' || true
exit "$result"
