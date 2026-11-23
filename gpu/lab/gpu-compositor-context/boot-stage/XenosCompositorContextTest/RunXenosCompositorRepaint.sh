#!/bin/sh
set -eu
export LC_ALL=C
unset GL_RESOURCES XENOS_GLD_COMPOSITOR_TEST DYLD_INSERT_LIBRARIES DYLD_FRAMEWORK_PATH DYLD_LIBRARY_PATH
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
mode=${1:---baseline}
case "$mode" in
    --baseline|--repaint) ;;
    *) echo "Usage: $0 [--baseline|--repaint]" >&2; exit 2 ;;
esac
windowserver_pid=$(ps -axww -o pid,command |
    awk '$2 ~ /\/WindowServer$/ && $3 == "-daemon" { print $1 }')
case "$windowserver_pid" in
    ""|*[!0-9]*) echo "Could not identify a single Tiger WindowServer process." >&2; exit 1 ;;
esac
if [ "$mode" = --baseline ]; then
    exec "$here/XenosCompositorRepaintCheck" "$mode" "$windowserver_pid"
fi
test "$here" = /Volumes/OSX360BOOT/XenosCompositorContextTest
/sbin/mount | grep -Fq ' on /Volumes/OSX360BOOT ('
/usr/sbin/kextstat | grep -Fq 'fish.goldfish64.osx360.XenonCoreGraphics (0.0.5d45)'
name=GLDriverXenosCompositor
staged="$here/$name.bundle/Contents/MacOS/$name"
installed="/System/Library/Extensions/$name.bundle/Contents/MacOS/$name"
if ! cmp -s "$staged" "$installed"; then
    echo "The matching compositor GLD must be installed before the repaint run." >&2
    exit 1
fi
arm="$here/repaint-probe.arm"
if ! ( set -C; umask 022; : > "$arm" ); then
    echo "The repaint marker already exists or could not be created." >&2
    exit 1
fi
trap 'rm -f "$arm"' 0
trap 'exit 1' HUP INT TERM
"$here/XenosCompositorRepaintCheck" "$mode" "$windowserver_pid"
