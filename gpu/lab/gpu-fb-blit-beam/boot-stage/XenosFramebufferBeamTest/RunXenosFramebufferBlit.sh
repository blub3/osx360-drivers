#!/bin/sh
set -eu
export LC_ALL=C
unset GL_RESOURCES XENOS_GLD_COMPOSITOR_RETRY_TEST XENOS_LEGACY_BLIT_CONTROL DYLD_INSERT_LIBRARIES DYLD_FRAMEWORK_PATH DYLD_LIBRARY_PATH
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
mode=${1:---baseline}
case "$mode" in
    --control|--lookup|--baseline|--repaint) ;;
    *) echo "Usage: $0 [--control|--lookup|--baseline|--repaint]" >&2; exit 2 ;;
esac
plugin=XenosBeamBlit.plugin
if [ "$mode" = --lookup ]; then
    XENOS_LEGACY_BLIT_CONTROL=1 exec "$here/XenosFramebufferBlitCheck" --lookup
fi
if [ "$mode" = --control ]; then
    scratch=$(mktemp -d /tmp/xenos-fb-blit.XXXXXXXX)
    trap 'rm -rf "$scratch"' 0
    trap 'exit 1' HUP INT TERM
    status=0
    XENOS_LEGACY_BLIT_CONTROL=1 "$here/XenosFramebufferBlitCheck" --control "$here/$plugin" > "$scratch/control" || status=$?
    cat "$scratch/control"
    test "$status" -eq 0
    grep -Fq "XenosFramebufferBlit timing PASS" "$scratch/control"
    grep -Eq '^XenosBeamBlit PASS source=fill pid=[0-9]+ pixels=1024 status=0x00000000$' "$scratch/control"
    exit
fi
windowserver_pid=$(ps -axww -o pid,command |
    awk '$2 ~ /\/WindowServer$/ && $3 == "-daemon" { print $1 }')
case "$windowserver_pid" in
    ""|*[!0-9]*) echo "Could not identify a single Tiger WindowServer process." >&2; exit 1 ;;
esac
if [ "$mode" = --baseline ]; then
    exec "$here/XenosFramebufferRepaintCheck" "$mode" "$windowserver_pid"
fi
boot=/Volumes/OSX360BOOT
test "$here" = "$boot/XenosFramebufferBeamTest"
/sbin/mount | grep -Fq " on $boot ("
if ! /usr/sbin/ioreg -w0 -c XenonFramebufferTest | grep -Fq "$plugin"; then
    echo "The running framebuffer has no Xenos blit registration. Cold boot into gpufbblitbeam first." >&2
    exit 1
fi
for name in Info.plist MacOS/XenosBeamBlit; do
    if ! cmp -s "$here/$plugin/Contents/$name" "/System/Library/Extensions/$plugin/Contents/$name"; then
        echo "The matching Tiger beam-retry plug-in must be installed." >&2
        exit 1
    fi
done
/usr/sbin/kextstat | grep -Fq 'fish.goldfish64.osx360.XenonCoreGraphics (0.0.5d45)'
XENOS_LEGACY_BLIT_CONTROL=1 "$here/XenosFramebufferBlitCheck" --lookup
scratch=$(mktemp -d /tmp/xenos-fb-blit.XXXXXXXX)
trap 'rm -rf "$scratch"' 0
trap 'exit 1' HUP INT TERM
guard="$here/repaint-run.lock"
mkdir "$guard"
arm="$here/repaint-blit.arm"
if [ -e "$arm" ]; then
    rmdir "$guard"
    echo "A repaint marker already exists." >&2
    exit 1
fi
trap 'rm -f "$arm"; rmdir "$guard"; rm -rf "$scratch"' 0
log=/var/log/system.log
test -r "$log"
tail -1500 "$log" | grep -E "XenosBeamBlit beam pid=$windowserver_pid " | tail -1 || :
log_offset=$(wc -c < "$log" | tr -d ' ')
status=0
"$here/XenosFramebufferRepaintCheck" "$mode" "$windowserver_pid" || status=$?
for retry in 1 2 3 4 5; do
    tail -c "+$((log_offset + 1))" "$log" | grep -F XenosBeamBlit > "$scratch/repaint" || :
    if grep -Eq "XenosBeamBlit (PASS|FAIL) source=(fill|copy|copy-region) pid=$windowserver_pid " "$scratch/repaint"; then
        break
    fi
    sleep 1
done
cat "$scratch/repaint"
test "$status" -eq 0
grep -Eq "XenosBeamBlit PASS source=(fill|copy|copy-region) pid=$windowserver_pid pixels=1024 status=0x00000000$" "$scratch/repaint"
test ! -e "$arm"
echo "WindowServer callback, GPU fence and readback PASS"
