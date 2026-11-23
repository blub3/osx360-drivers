#!/bin/sh
set -eu
export LC_ALL=C
boot=/Volumes/OSX360BOOT
mode=${1:---startup}
case "$mode" in --startup|--repaint) ;; *) exit 2 ;; esac

if ! /usr/sbin/kextstat | grep -Fq 'fish.goldfish64.osx360.XenonCoreGraphics (0.0.5d46)'; then
    echo "XenonCoreGraphics 0.0.5d46 is not running. Cold boot gpuringfix first." >&2
    exit 1
fi
gpu=$(/usr/sbin/ioreg -w0 -c XenosController)
printf '%s\n' "$gpu" | grep -E '"XenosRingStartup(Ready|Attempts)"|"XenosWindowServerFramebufferTrace"'
stamp=$(date -r "$(/usr/sbin/sysctl -n kern.boottime)" '+%b %e %T')
startup=$(awk -v stamp="$stamp" '
    substr($0,1,15) >= stamp && /XenosRingStartup:|XenosMarker:|XenosWindowServerFramebuffer: fence/ {print}
' /var/log/system.log)
printf '%s\n' "$startup"
if ! printf '%s\n' "$gpu" | grep -Eq '"XenosRingStartupReady" = Yes'; then
    echo "GPU startup did not pass. The repaint is unarmed." >&2
    exit 1
fi
for phase in command-processor framebuffer; do
    printf '%s\n' "$startup" | grep -Eq "XenosMarker: PASS .*phase=$phase$"
done
trace=$(printf '%s\n' "$gpu" | awk -F '[<>]' '/"XenosWindowServerFramebufferTrace"/ { print $2 }')
if ! printf '%s\n' "$trace" | awk '
    length($0) == 136 && substr($0,1,8) == "00000002" &&
    substr($0,41,24) == "000000010000040000000000" &&
    substr($0,105,24) == "000000010000040000000000" { ready=1 }
    END { exit !ready }
'; then
    echo "GPU startup markers passed, but both WindowServer clears are still required." >&2
    exit 1
fi
echo "Xenos GPU startup markers and both WindowServer clear fences PASS"
if [ "$mode" = --repaint ]; then
    exec sh "$boot/XenosWindowBatchTest/RunXenosWindowBatch.sh" --repaint
fi
