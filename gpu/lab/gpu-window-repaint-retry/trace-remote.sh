set -eu
export PATH=/usr/bin:/bin:/usr/sbin:/sbin LC_ALL=C
boot=/Volumes/OSX360BOOT
test "$(md5 -q "$boot/kboot.conf")" = 348ca1649e90bea8a13541525f9ac0d6
test "$(md5 -q /System/Library/Extensions/XenosWinBlit.plugin/Contents/MacOS/XenosWinBlit)" = 4e22f02494f032f7a815d3e15ccbd401
cd "$boot/XenosWindowRepaintTest"
test "$(md5 -q RunXenosWindowRepaint.sh)" = 2e5ef7008514c68ed7825567df47fc69
test "$(md5 -q XenosWindowRepaintCheck)" = a3e573ba20dd0b1d4bbafbfac95ba3cb
beam="$boot/XenosFramebufferBeamTest/XenosFramebufferRepaintCheck"
test "$(md5 -q "$beam")" = c86a8816a2dad8b199cecee938df8d11
pid=$(ps -axww -o pid,command | awk "\$2 ~ /\\/WindowServer\$/ && \$3 == \"-daemon\" { print \$1 }")
case "$pid" in ""|*[!0-9]*) exit 1 ;; esac
work=$(mktemp -d /tmp/xenos-repaint-ktrace.XXXXXXXX)
tracing=no
cleanup() {
    if [ "$tracing" = yes ]; then
        /usr/bin/ktrace -c -t cn -p "$pid" || :
    fi
    rm -rf "$work"
}
trap cleanup 0
trap "exit 1" HUP INT TERM
trace() {
    label=$1
    shift
    result=0
    /usr/bin/ktrace -f "$work/$label" -t cn -p "$pid"
    tracing=yes
    /usr/bin/sudo -u bblankenhagen "$@" || result=$?
    /usr/bin/ktrace -c -t cn -p "$pid"
    tracing=no
    printf "WindowServer trace workload=%s pid=%s test_status=%s\n" "$label" "$pid" "$result"
    /usr/bin/kdump -f "$work/$label" -t cn |
        awk "/CALL  +open[(_]/ { pending=0; call=\$0 }
             /NAMI  .*repaint-window[.]arm/ { print call; print; pending=1; n++ }
             /RET   +open/ && pending { print; pending=0 }
             END { print \"Arm path lookups:\",n+0 }"
}
trace 16x16 /bin/sh RunXenosWindowRepaint.sh --repaint
trace overlapping "$beam" --baseline "$pid"
test ! -e repaint-window.arm && test ! -e repaint-window.lock
test "$(ps -axww -o pid,command | awk "\$2 ~ /\\/WindowServer\$/ && \$3 == \"-daemon\" { print \$1 }")" = "$pid"
echo "WindowServer blit trace complete."
