#!/bin/sh
set -eu
export LC_ALL=C
unset XENOS_LEGACY_BLIT_CONTROL GL_RESOURCES DYLD_INSERT_LIBRARIES DYLD_FRAMEWORK_PATH DYLD_LIBRARY_PATH
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
mode=${1:---baseline}
plugin=XenosWinAll.plugin
case "$mode" in
    --control)
        XENOS_LEGACY_BLIT_CONTROL=1 "$here/XenosWindowPluginCheck" --control "$here/$plugin"
        exec "$here/XenosWindowRepaintCheck" --control
        ;;
    --baseline|--overlap-baseline|--mapping-policy) exec "$here/XenosWindowRepaintCheck" "$mode" ;;
    --lookup) XENOS_LEGACY_BLIT_CONTROL=1 exec "$here/XenosWindowRepaintCheck" --lookup ;;
    --repaint) ;;
    *) echo "Usage: $0 [--control|--baseline|--overlap-baseline|--mapping-policy|--lookup|--repaint]" >&2; exit 2 ;;
esac
boot=/Volumes/OSX360BOOT
test "$here" = "$boot/XenosWindowFullTest"
/sbin/mount | grep -Fq " on $boot ("
if ! /usr/sbin/ioreg -w0 -c XenonFramebufferTest | grep -Fq "$plugin"; then
    echo "The running framebuffer has no window-copy registration. Cold boot gpuwinfull first." >&2
    exit 1
fi
for name in Info.plist MacOS/XenosWinAll; do
    if ! cmp -s "$here/$plugin/Contents/$name" "/System/Library/Extensions/$plugin/Contents/$name"; then
        echo "The matching Xenos window-copy plug-in must be installed." >&2
        exit 1
    fi
done
/usr/sbin/kextstat | grep -Fq 'fish.goldfish64.osx360.XenonFramebufferTest (0.0.5d3)'
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
"$here/XenosWindowRepaintCheck" --repaint "$pid" > "$scratch/app" || status=$?
cat "$scratch/app"
for retry in 1 2 3 4 5; do
    tail -c "+$((log_offset + 1))" "$log" | grep -F "XenosWinAll" > "$scratch/driver" || :
    if grep -Eq "XenosWinAll (PASS|FAIL) pid=$pid " "$scratch/driver"; then
        break
    fi
    sleep 1
done
cat "$scratch/driver"
test "$status" -eq 0
witness=$(awk '/^XenosWindowRepaint PASS mode=repaint / {print $4,$5,$6,$9,$10}' "$scratch/app")
read -r src dst hash ticket fence <<EOF
$witness
EOF
test -n "$fence"
awk -v tag="XenosWinAll PASS pid=$pid " -v src="$src" -v dst="$dst" \
    -v hash="$hash" -v ticket="$ticket" -v fence="$fence" '
    index($0, tag) && index($0, " " src " " dst " " hash " " ticket " " fence " ") {
        for (i in field)
            delete field[i]
        for (i = 1; i <= NF; ++i) {
            if (split($i, pair, "=") == 2)
                field[pair[1]] = pair[2]
        }
        area = field["area"] + 0
        if ((field["kind"] == "copy" || field["kind"] == "copy-region") &&
            field["reason"] == "verified" && field["map_status"] == "0x00000000" &&
            field["batches"] == 2 && field["first_fence"] > 0 &&
            field["first_fence"] + 0 < field["fence"] + 0 &&
            area > 256 && area <= 512 && area == field["gpu_pixels"] + 0 &&
            area == field["screen_pixels"] + 0)
            verified = 1
    }
    END { exit !verified }
' "$scratch/driver"
test ! -e "$arm"
test "$(ps -axww -o pid,command | awk '$2 ~ /\/WindowServer$/ && $3 == "-daemon" {print $1}')" = "$pid"
echo "WindowServer full clipped GPU copy, two ordered fences and on-screen pixels PASS"
