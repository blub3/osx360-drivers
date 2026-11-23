#!/bin/sh
set -eu
export LC_ALL=C
unset GL_RESOURCES XENOS_GLD_COMPOSITOR_RETRY_TEST XENOS_GLD_COMPOSITOR_TEST
unset DYLD_INSERT_LIBRARIES DYLD_FRAMEWORK_PATH DYLD_LIBRARY_PATH
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
test "$here" = /Volumes/OSX360BOOT/XenosCompositorRetryTest
/sbin/mount | grep -Fq ' on /Volumes/OSX360BOOT ('
/usr/sbin/kextstat | grep -Fq 'fish.goldfish64.osx360.XenonCoreGraphics (0.0.5d45)'
name=GLDriverXenosCompositorRetry
/usr/sbin/ioreg -w0 -c XenonFramebufferTest | grep -Fq "\"IOGLBundleName\" = \"$name\""
staged="$here/$name.bundle/Contents/MacOS/$name"
installed="/System/Library/Extensions/$name.bundle/Contents/MacOS/$name"
if ! cmp -s "$staged" "$installed"; then
    echo "Install the staged compositor retry GLD before the transition run." >&2
    exit 1
fi
pid=$(ps -axww -o pid,command |
    awk '$2 ~ /\/WindowServer$/ && $3 == "-daemon" { print $1 }')
case "$pid" in ''|*[!0-9]*) echo "Could not identify WindowServer." >&2; exit 1 ;; esac
owner=$(printf '%08x' "$pid")
completion() {
    /usr/sbin/ioreg -w0 -c XenosController |
        sed -n 's/.*"XenosGPUCompletion" = <\([0-9a-f]*\)>.*/\1/p'
}
before=$(completion)
test ! -e "$here/repaint-probe.arm"
arm="$here/transition-probe.arm"
if ! ( set -C; umask 022; : > "$arm" ); then
    echo "The transition marker already exists or could not be created." >&2
    exit 1
fi
trap 'rm -f "$arm"' 0
trap 'exit 1' HUP INT TERM
"$here/XenosTransitionProbe"
for attempt in 1 2 3 4 5; do
    after=$(completion)
    if [ "$after" != "$before" ] &&
       printf '%s\n' "$after" | awk -v owner="$owner" '
           length($0) == 56 && substr($0,9,8) == owner &&
           substr($0,17,8) != "00000000" && substr($0,17,8) == substr($0,25,8) &&
           substr($0,33) == "000000010000040000000000" { pass=1 }
           END { exit !pass }'; then
        ticket=$(printf '%s' "$after" | cut -c17-24)
        ticket=$((0x$ticket))
        event="XenosCompositorRetryGLD source=transition-format pid=$pid frame=1 ticket=$ticket fence=$ticket pixels=1024"
        if tail -200 /var/log/system.log | grep -F "$event"; then
            printf 'PASS WindowServer transition format fence pid=%s ticket=%s pixels=1024\n' "$pid" "$ticket"
            exit 0
        fi
    fi
    sleep 1
done
printf 'FAIL WindowServer transition format fence pid=%s before=%s after=%s\n' "$pid" "$before" "$after"
exit 1
