#!/bin/sh
set -eu
export LC_ALL=C
unset XENOS_LEGACY_BLIT_CONTROL GL_RESOURCES DYLD_INSERT_LIBRARIES DYLD_FRAMEWORK_PATH DYLD_LIBRARY_PATH
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
boot=/Volumes/OSX360BOOT
name=XenosWinAuto
baseline=--baseline
move=unarmed
area=324
tiles=1
budget=350
if [ "${here##*/}" = XenosWindowOverlapTest ]; then
    name=XenosWinOverlap
    baseline=--overlap-baseline
    move=unarmed-overlap
    area=7252
    tiles=8
    budget=1500
fi
plugin=$name.plugin
check="$here/XenosWindowRepaintCheck"

case ${1:---repaint} in
    --plugin-control)
        XENOS_LEGACY_BLIT_CONTROL=1 exec "$here/XenosWindowPluginCheck" --control "$here/$plugin"
        ;;
    --baseline)
        exec "$check" "$baseline"
        ;;
    --repaint) ;;
    *) echo "Usage: $0 [--plugin-control|--baseline|--repaint]" >&2; exit 2 ;;
esac

framebuffer=$(/usr/sbin/ioreg -w0 -c XenonFramebufferTest)
printf '%s\n' "$framebuffer" | grep -Fq "$plugin"
/usr/sbin/kextstat | grep -Fq 'fish.goldfish64.osx360.XenonCoreGraphics (0.0.5d48)'
/usr/sbin/ioreg -w0 -c XenosController | grep -Fq '"XenosRingStartupReady" = Yes'
for file in Info.plist "MacOS/$name"; do
    cmp "$here/$plugin/Contents/$file" "/System/Library/Extensions/$plugin/Contents/$file"
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
    "$check" "--$move" "$pid" > "$scratch/app" || status=$?
    cat "$scratch/app"
    test "$status" -eq 0
    fence=$(awk -v mode="mode=$move" '$1 == "XenosWindowRepaint" && $2 == "PASS" && $3 == mode {
        for (i = 1; i <= NF; i++)
            if ($i ~ /^fence=/) { sub(/^fence=/, "", $i); print $i }
    }' "$scratch/app")
    case "$fence" in ''|*[!0-9]*) exit 1 ;; esac
    test "$fence" -gt "$last_fence"
    last_fence=$fence

    verified=0
    for retry in 1 2 3 4 5; do
        tail -c "+$((offset + 1))" "$log" > "$scratch/driver"
        if awk -v pid="$pid" -v fence="$fence" -v name="$name" \
               -v area="$area" -v tiles="$tiles" -v budget="$budget" '
            index($0, name " PASS ") || /XenosTileCapture: PASS / {
                for (key in field)
                    delete field[key]
                for (i = 1; i <= NF; ++i)
                    if (split($i, pair, "=") == 2)
                        field[pair[1]] = pair[2]
                if (/XenosTileCapture: PASS /) {
                    if (field["ticket"] == field["fence"] && field["pixels"] == 1024 && field["mismatches"] == 0) {
                        kernel[field["fence"]]++
                        receipt[field["fence"]] = $0
                    }
                    next
                }
                if (field["ticket"] == fence && field["fence"] == fence && field["pid"] == pid &&
                    field["area"] == area && field["gpu_pixels"] == area && field["screen_pixels"] == area &&
                    field["committed"] == 1 && field["reason"] == "verified" &&
                    field["gpu_ms"] ~ /^[0-9]+$/ && field["gpu_ms"] + 0 < budget &&
                    field["batches"] == tiles && field["first_fence"] + 0 == fence - tiles + 1 &&
                    split(field["fences"], order, ",") == tiles) {
                    for (i = 1; i <= tiles; ++i)
                        if (order[i] !~ /^[0-9]+$/ || order[i] + 0 != fence - tiles + i)
                            next
                    plugin++
                    repaint = $0
                }
            }
            END {
                if (plugin != 1)
                    exit 1
                for (i = fence - tiles + 1; i <= fence; ++i) {
                    if (kernel[i] != 1)
                        exit 1
                    print receipt[i]
                }
                print repaint
            }
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
