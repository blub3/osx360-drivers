#!/bin/sh
set -eu
export LC_ALL=C
unset XENOS_LEGACY_BLIT_CONTROL GL_RESOURCES DYLD_INSERT_LIBRARIES DYLD_FRAMEWORK_PATH DYLD_LIBRARY_PATH
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
boot=/Volumes/OSX360BOOT
plugin=XenosWinAuto.plugin
check="$here/XenosWindowRepaintCheck"

case ${1:---repaint} in
    --plugin-control)
        XENOS_LEGACY_BLIT_CONTROL=1 exec "$here/XenosWindowPluginCheck" --control "$here/$plugin"
        ;;
    --baseline)
        exec "$check" --baseline
        ;;
    --repaint) ;;
    *) echo "Usage: $0 [--plugin-control|--baseline|--repaint]" >&2; exit 2 ;;
esac

framebuffer=$(/usr/sbin/ioreg -w0 -c XenonFramebufferTest)
printf '%s\n' "$framebuffer" | grep -Fq "$plugin"
/usr/sbin/kextstat | grep -Fq 'fish.goldfish64.osx360.XenonCoreGraphics (0.0.5d48)'
/usr/sbin/ioreg -w0 -c XenosController | grep -Fq '"XenosRingStartupReady" = Yes'
for name in Info.plist MacOS/XenosWinAuto; do
    cmp "$here/$plugin/Contents/$name" "/System/Library/Extensions/$plugin/Contents/$name"
done
"$check" --mapping-policy

windowserver_pid() {
    ps -axww -o pid,command | awk '$2 ~ /\/WindowServer$/ && $3 == "-daemon" {print $1}'
}

no_arm() {
    test ! -e "$boot/XenosWindowTileTest/repaint-window.arm"
    test ! -e "$boot/XenosWindowTileTest/repaint-window.lock"
}

pid=$(windowserver_pid)
case "$pid" in ''|*[!0-9]*) exit 1 ;; esac
scratch=$(mktemp -d /tmp/xenos-winauto-check.XXXXXXXX)
trap 'rm -rf "$scratch"' 0
trap 'exit 1' HUP INT TERM
log=/var/log/system.log
last_fence=0
for run in 1 2; do
    sleep 2
    no_arm
    offset=$(wc -c < "$log" | tr -d ' ')
    status=0
    "$check" --unarmed "$pid" > "$scratch/app" || status=$?
    cat "$scratch/app"
    test "$status" -eq 0
    fence=$(awk '/^XenosWindowRepaint PASS mode=unarmed / {
        for (i = 1; i <= NF; i++)
            if ($i ~ /^fence=/) { sub(/^fence=/, "", $i); print $i }
    }' "$scratch/app")
    case "$fence" in ''|*[!0-9]*) exit 1 ;; esac
    test "$fence" -gt "$last_fence"
    last_fence=$fence

    verified=0
    for retry in 1 2 3 4 5; do
        tail -c "+$((offset + 1))" "$log" > "$scratch/driver"
        if awk -v pid="$pid" -v fence="$fence" '
            /XenosWinAuto PASS |XenosTileCapture: PASS / {
                for (key in field)
                    delete field[key]
                for (i = 1; i <= NF; ++i)
                    if (split($i, pair, "=") == 2)
                        field[pair[1]] = pair[2]
                if (field["ticket"] != fence || field["fence"] != fence)
                    next
                if (/XenosTileCapture: PASS / && field["pixels"] == 1024 && field["mismatches"] == 0) {
                    kernel++
                    print
                }
                if (/XenosWinAuto PASS / && field["pid"] == pid && field["area"] == 324 &&
                    field["gpu_pixels"] == 324 && field["screen_pixels"] == 324 &&
                    field["committed"] == 1 && field["reason"] == "verified") {
                    plugin++
                    print
                }
            }
            END { exit !(kernel == 1 && plugin == 1) }
        ' "$scratch/driver" > "$scratch/receipts"; then
            verified=1
            cat "$scratch/receipts"
            break
        fi
        sleep 1
    done
    test "$verified" -eq 1
    no_arm
    test "$(windowserver_pid)" = "$pid"
done
echo "Two unarmed WindowServer window moves, increasing GPU fences and pixel readbacks PASS"
