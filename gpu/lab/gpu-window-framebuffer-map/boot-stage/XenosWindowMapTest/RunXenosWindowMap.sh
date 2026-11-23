#!/bin/sh
set -eu
export LC_ALL=C
unset XENOS_LEGACY_BLIT_CONTROL GL_RESOURCES DYLD_INSERT_LIBRARIES DYLD_FRAMEWORK_PATH DYLD_LIBRARY_PATH
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
mode=${1:---baseline}
plugin=XenosWinMap.plugin
case "$mode" in
    --control)
        XENOS_LEGACY_BLIT_CONTROL=1 "$here/XenosWindowPluginCheck" --control "$here/$plugin"
        exec "$here/XenosWindowRepaintCheck" --control
        ;;
    --baseline|--overlap-baseline|--mapping-baseline) exec "$here/XenosWindowRepaintCheck" "$mode" ;;
    --lookup) XENOS_LEGACY_BLIT_CONTROL=1 exec "$here/XenosWindowRepaintCheck" --lookup ;;
    --repaint) ;;
    *) echo "Usage: $0 [--control|--baseline|--overlap-baseline|--mapping-baseline|--lookup|--repaint]" >&2; exit 2 ;;
esac
boot=/Volumes/OSX360BOOT
test "$here" = "$boot/XenosWindowMapTest"
/sbin/mount | grep -Fq " on $boot ("
if ! /usr/sbin/ioreg -w0 -c XenonFramebufferTest | grep -Fq "$plugin"; then
    echo "The running framebuffer has no window-copy registration. Cold boot gpuwinmap first." >&2
    exit 1
fi
for name in Info.plist MacOS/XenosWinMap; do
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
if [ "$status" -ne 0 ]; then
    status=0
    "$here/XenosWindowRepaintCheck" --overlap "$pid" >> "$scratch/app" || status=$?
fi
cat "$scratch/app"
for retry in 1 2 3 4 5; do
    tail -c "+$((log_offset + 1))" "$log" | grep -F "XenosWinMap" > "$scratch/driver" || :
    if grep -Eq "XenosWinMap (PASS|FAIL) pid=$pid " "$scratch/driver"; then
        break
    fi
    sleep 1
done
cat "$scratch/driver"
test "$status" -eq 0
witness=$(awk '/^XenosWindowRepaint PASS mode=(repaint|overlap) / {print $4,$5,$6,$9,$10}' "$scratch/app")
read -r src dst hash ticket fence <<EOF
$witness
EOF
test -n "$fence"
grep -Eq "XenosWinMap PASS pid=$pid kind=(copy|copy-region) $src $dst $hash $ticket $fence pixels=256 rects=[0-9]+ area=[0-9]+ reason=verified map_status=0x00000000$" "$scratch/driver"
test ! -e "$arm"
test "$(ps -axww -o pid,command | awk '$2 ~ /\/WindowServer$/ && $3 == "-daemon" {print $1}')" = "$pid"
echo "WindowServer GPU copy, matching fence and on-screen pixels PASS"
