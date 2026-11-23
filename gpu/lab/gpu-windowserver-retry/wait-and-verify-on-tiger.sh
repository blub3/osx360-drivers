#!/bin/sh
set -eu
cd "$(dirname "$0")"
old_boot=$1
deadline=$(($(date +%s) + 780))
ready=
while [ "$(date +%s)" -lt "$deadline" ]; do
    if state=$(../gpu-cgl-discovery/tiger-ssh '
        boot=$(/usr/sbin/sysctl -n kern.boottime) || exit 1
        test -f /Volumes/OSX360BOOT/XenosWindowServerRetryTest/XenosWindowServerCheck || exit 1
        /bin/ps -axww -o pid,command |
            /usr/bin/awk '\''$2 ~ /\/WindowServer$/ && $3 == "-daemon" { found=1 } END { exit !found }'\'' || exit 1
        printf "%s\n" "$boot"
    ' 2>/dev/null) && [ -n "$state" ] && [ "$state" != "$old_boot" ]; then
        ready=$state
        break
    fi
    sleep 10
done
if [ -z "$ready" ]; then
    echo "Timed out waiting for a new Tiger boot with WindowServer and OSX360BOOT available."
    exit 1
fi
printf 'Tiger boot time %s (previous %s)\n' "$ready" "$old_boot"
../gpu-cgl-discovery/tiger-ssh /bin/sh <<'REMOTE'
set -eu
/usr/sbin/sysctl -n kern.boottime
cd /Volumes/OSX360BOOT
grep '^default=' kboot.conf
if ! /usr/sbin/kextstat | grep -F 'fish.goldfish64.osx360.XenonCoreGraphics (0.0.5d40)'; then
    /usr/sbin/kextstat | grep -i Xenon || :
    /sbin/dmesg | grep -Ei 'XenonCoreGraphics|proc_selfpid|XenosWindowServer' || :
    echo "XenonCoreGraphics 0.0.5d40 is absent."
    exit 3
fi
cd XenosWindowServerRetryTest
association=0
control_status=0
ownership_status=0
native=0
/bin/sh RunXenosWindowServer.sh --association || association=$?
control=$(/bin/sh RunXenosWindowServer.sh --control 2>&1) || control_status=$?
printf '%s\n' "$control"
printf '%s\n' "$control" | grep -F 'owner_trace=verified' >/dev/null || ownership_status=$?
/bin/sh RunXenosWindowServer.sh --require-native || native=$?
printf 'Association status %s, control status %s, owner trace status %s, native acceptance status %s\n' \
    "$association" "$control_status" "$ownership_status" "$native"
/sbin/dmesg | grep -E 'XenosWindowServer|native HDMI' | tail -20 || :
test "$association" -eq 0 && test "$control_status" -eq 0 && test "$ownership_status" -eq 0
REMOTE
