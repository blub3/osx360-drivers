#!/bin/bash
# Restart, stop or start the Xbox's GUI (WindowServer and the autologin session) over SSH, without
# rebooting. GPU plan step 0.2, docs/gpu-driver-plan.md.
#
# STATUS 2026-11-25: NOT RELIABLE YET. restart worked once (desktop back in 7.7 s) and failed on the
# next run; stop and start are untested. A failed run leaves the Xbox without a desktop; recover
# with 'ssh xbox sudo reboot' (desktop back in about 2 minutes). See the step 0.2 log entry.
#
#   xbox-gui.sh status    WindowServer, loginwindow, Finder and Dock PIDs
#   xbox-gui.sh restart   end the session and WindowServer, start a new WindowServer, wait for Finder and Dock
#   xbox-gui.sh stop      keep the GUI down: console line in /etc/ttys off, end loginwindow and WindowServer
#   xbox-gui.sh start     undo stop: restore /etc/ttys, start loginwindow and WindowServer
#
# Tiger detail: after loginwindow is killed, WindowServer exits too, and the new loginwindow does not
# start a new one (it waits on the old com.apple.windowserver port, or aborts in a loop if the name is
# gone). The script starts WindowServer -daemon itself, through /usr/libexec/StartupItemContext so that
# it registers in the system's bootstrap namespace, not in the SSH session's. Open applications are
# quit without saving.
S="-o IgnoreUnknown=UseKeychain -o BatchMode=yes -o ConnectTimeout=10"
cmd=${1:-status}

remote=$(cat <<'EOF'
WS=/System/Library/Frameworks/ApplicationServices.framework/Frameworks/CoreGraphics.framework/Resources/WindowServer
now() { perl -MTime::HiRes=time -e 'printf "%.1f", time'; }
since() { perl -MTime::HiRes=time -e "printf q{%.1f}, time-$1"; }
nap() { perl -e 'select(undef,undef,undef,0.5)'; }
pids() { ps -axww -o pid,command | awk '$2 ~ /\/CoreGraphics.framework\/Resources\/WindowServer$/ && $3 == "-daemon" {w=$1} $2 ~ /\/loginwindow.app\/Contents\/MacOS\/loginwindow$/ {l=$1} $2 ~ /\/Finder.app\/Contents\/MacOS\/Finder$/ {f=$1} $2 ~ /\/Dock.app\/Contents\/MacOS\/Dock$/ {d=$1} END{print "ws=" w " lw=" l " finder=" f " dock=" d}'; }
field() { echo "$1" | sed -n "s/.*$2=\([0-9]*\).*/\1/p"; }
wait_gone() { i=0; while [ -n "$(field "$(pids)" ws)" ] && [ $i -lt 60 ]; do nap; i=$((i+1)); done; }
wait_desktop() { i=0; while [ $i -lt 240 ]; do p=$(pids); [ -n "$(field "$p" finder)" ] && [ -n "$(field "$p" dock)" ] && return 0; nap; i=$((i+1)); done; return 1; }
start_ws() { [ -n "$(field "$(pids)" ws)" ] || sudo sh -c "nohup /usr/libexec/StartupItemContext $WS -daemon >/dev/null 2>&1 &"; }
case "$CMD" in
  status) pids ;;
  restart)
    echo "before $(pids)"; t0=$(now)
    sudo killall loginwindow; wait_gone; echo "ws_gone_s=$(since $t0)"
    start_ws
    if wait_desktop; then echo "after $(pids) desktop_s=$(since $t0)"; else echo "FAIL no desktop $(pids)"; exit 1; fi ;;
  stop)
    [ -f /etc/ttys.osx360-gui.bak ] || sudo cp -p /etc/ttys /etc/ttys.osx360-gui.bak
    sudo sed -i '' 's|^\(console[[:space:]]*"/System/Library/CoreServices/loginwindow.app/Contents/MacOS/loginwindow"[[:space:]]*vt100[[:space:]]*\)on |\1off |' /etc/ttys
    grep -q '^console.*loginwindow.*vt100[[:space:]]*off ' /etc/ttys || { echo "FAIL ttys edit"; exit 1; }
    sudo kill -HUP 1; t0=$(now); sudo killall loginwindow 2>/dev/null; wait_gone
    i=0; while [ $i -lt 20 ]; do nap; i=$((i+1)); done
    echo "stopped $(pids) after $(since $t0) s" ;;
  start)
    [ -f /etc/ttys.osx360-gui.bak ] && sudo cp -p /etc/ttys.osx360-gui.bak /etc/ttys && sudo rm /etc/ttys.osx360-gui.bak
    grep -q '^console.*loginwindow.*vt100[[:space:]]*on ' /etc/ttys || { echo "FAIL ttys not restored"; exit 1; }
    t0=$(now); sudo kill -HUP 1
    i=0; while [ -z "$(field "$(pids)" lw)" ] && [ $i -lt 60 ]; do nap; i=$((i+1)); done
    start_ws
    if wait_desktop; then echo "started $(pids) desktop_s=$(since $t0)"; else echo "FAIL no desktop $(pids)"; exit 1; fi ;;
  *) echo "usage: status | restart | stop | start"; exit 2 ;;
esac
EOF
)
timeout 240 ssh $S xbox "CMD=$cmd; $remote"
