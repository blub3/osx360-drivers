#!/bin/sh
set -eu
export LC_ALL=C
unset XENOS_LEGACY_BLIT_CONTROL GL_RESOURCES DYLD_INSERT_LIBRARIES DYLD_FRAMEWORK_PATH DYLD_LIBRARY_PATH
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
mode=${1:---overlap-baseline}
plugin=XenosWinTile.plugin
case "$mode" in
    --plugin-control)
        XENOS_LEGACY_BLIT_CONTROL=1 exec "$here/XenosWindowPluginCheck" --control "$here/$plugin"
        ;;
    --control)
        /usr/sbin/kextstat | grep -Fq 'fish.goldfish64.osx360.XenonCoreGraphics (0.0.5d48)'
        sh "$here/RunXenosWindowTile.sh" --plugin-control
        exec "$here/XenosWindowRepaintCheck" --control
        ;;
    --startup) exec sh "$here/CheckXenosTileStartup.sh" ;;
    --baseline|--overlap-baseline|--mapping-policy) exec "$here/XenosWindowRepaintCheck" "$mode" ;;
    --lookup) XENOS_LEGACY_BLIT_CONTROL=1 exec "$here/XenosWindowRepaintCheck" --lookup ;;
    --repaint) ;;
    *) echo "Usage: $0 [--plugin-control|--control|--startup|--baseline|--overlap-baseline|--mapping-policy|--lookup|--repaint]" >&2; exit 2 ;;
esac
boot=/Volumes/OSX360BOOT
test "$here" = "$boot/XenosWindowTileTest"
/sbin/mount | grep -Fq " on $boot ("
if ! /usr/sbin/ioreg -w0 -c XenonFramebufferTest | grep -Fq "$plugin"; then
    echo "The running framebuffer has no window-copy registration. Cold boot gpuwintile first." >&2
    exit 1
fi
for name in Info.plist MacOS/XenosWinTile; do
    if ! cmp -s "$here/$plugin/Contents/$name" "/System/Library/Extensions/$plugin/Contents/$name"; then
        echo "The matching Xenos window-copy plug-in must be installed." >&2
        exit 1
    fi
done
/usr/sbin/kextstat | grep -Fq 'fish.goldfish64.osx360.XenonFramebufferTest (0.0.5d3)'
sh "$here/CheckXenosTileStartup.sh"
"$here/XenosWindowRepaintCheck" --mapping-policy
XENOS_LEGACY_BLIT_CONTROL=1 "$here/XenosWindowRepaintCheck" --lookup
pid=$(ps -axww -o pid,command | awk '$2 ~ /\/WindowServer$/ && $3 == "-daemon" {print $1}')
case "$pid" in ''|*[!0-9]*) exit 1 ;; esac
scratch=$(mktemp -d /tmp/xenos-window-repaint.XXXXXXXX)
trap 'rm -rf "$scratch"' 0
trap 'exit 1' HUP INT TERM
guard="$here/repaint-window.lock"
mkdir "$guard"
arm="$here/repaint-window.arm"
if [ -e "$arm" ]; then
    rmdir "$guard"
    echo "A repaint marker already exists." >&2
    exit 1
fi
trap 'rm -f "$arm"; rmdir "$guard"; rm -rf "$scratch"' 0
log=/var/log/system.log
test -r "$log"
log_offset=$(wc -c < "$log" | tr -d ' ')
status=0
"$here/XenosWindowRepaintCheck" --overlap "$pid" > "$scratch/app" || status=$?
cat "$scratch/app"
for retry in 1 2 3 4 5; do
    tail -c "+$((log_offset + 1))" "$log" | grep -E "XenosWinTile|XenosTileCapture:" > "$scratch/driver" || :
    if grep -Fq "XenosWinTile FAIL pid=$pid " "$scratch/driver"; then
        break
    fi
    if grep -Fq "XenosWinTile PASS pid=$pid " "$scratch/driver" &&
        [ "$(grep -c 'XenosTileCapture: PASS ' "$scratch/driver" || :)" -ge 8 ]; then
        break
    fi
    sleep 1
done
cat "$scratch/driver"
test "$status" -eq 0
witness=$(awk '/^XenosWindowRepaint PASS mode=overlap / {print $4,$5,$6,$9,$10,$NF}' "$scratch/app")
read -r src dst hash ticket fence move <<EOF
$witness
EOF
test -n "$fence"
move=${move#move_ms=}
case "$move" in ''|*[!0-9]*) exit 1 ;; esac
awk -v tag="XenosWinTile PASS pid=$pid " -v src="$src" -v dst="$dst" \
    -v hash="$hash" -v ticket="$ticket" -v fence="$fence" -v move="$move" '
    /XenosTileCapture: PASS / {
        for (i = 1; i <= NF; ++i) {
            if (split($i, pair, "=") == 2)
                receipt[pair[1]] = pair[2]
        }
        if (receipt["ticket"] ~ /^[0-9]+$/ && receipt["ticket"] == receipt["fence"] &&
            receipt["pixels"] == 1024 && receipt["mismatches"] == 0)
            captures[receipt["ticket"]]++
        for (i in receipt)
            delete receipt[i]
    }
    index($0, tag) && index($0, " " src " " dst " " hash " " ticket " " fence " ") {
        for (i in field)
            delete field[i]
        for (i = 1; i <= NF; ++i) {
            if (split($i, pair, "=") == 2)
                field[pair[1]] = pair[2]
        }
        area = field["area"] + 0
        count = split(field["fences"], order, ",")
        if ((field["kind"] != "copy" && field["kind"] != "copy-region") ||
            field["reason"] != "verified" || field["map_status"] != "0x00000000" ||
            area != 7252 || area != field["gpu_pixels"] + 0 || area != field["screen_pixels"] + 0 ||
            field["batches"] != 8 || count != field["batches"] + 0 ||
            field["first_fence"] != order[1] || field["fence"] != order[count] ||
            field["ticket"] != order[count] || field["gpu_ms"] !~ /^[0-9]+$/ ||
            field["gpu_ms"] + 0 <= 0 || field["gpu_ms"] + 0 >= 8000 || move + 0 <= 0 || order[1] + 0 <= 0)
            next
        for (i = 1; i <= count; ++i) {
            if (order[i] !~ /^[0-9]+$/ || (i > 1 && order[i] + 0 != order[i-1] + 1))
                next
        }
        expected = field["fences"]
        gpu = field["gpu_ms"] + 0
        verified = 1
    }
    END {
        if (!verified)
            exit 1
        count = split(expected, receipts, ",")
        for (i = 1; i <= count; ++i) {
            if (captures[receipts[i]] != 1)
                exit 1
        }
        baseline = 4276
        printf "XenosTileTiming gpu_ms=%d move_ms=%d reference_gpu_ms=%d saved_ms=%d speedup=%.2fx\n",
               gpu, move, baseline, baseline - gpu, baseline / gpu
    }
' "$scratch/driver"
test ! -e "$arm"
test "$(ps -axww -o pid,command | awk '$2 ~ /\/WindowServer$/ && $3 == "-daemon" {print $1}')" = "$pid"
echo "WindowServer overlapping GPU copy, eight ordered fences and full clipped pixels PASS"
