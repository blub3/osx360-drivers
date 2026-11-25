#!/bin/bash
# Restart, stop or start the Xbox's GUI (WindowServer and the autologin session) over SSH, without
# rebooting. GPU plan step 0.2, docs/gpu-driver-plan.md.
#
#   xbox-gui.sh status    WindowServer, loginwindow, Finder and Dock PIDs
#   xbox-gui.sh stop      GUI down: console line in /etc/ttys off, launchd ends loginwindow, WindowServer exits
#   xbox-gui.sh start     GUI up: start WindowServer, console line on, loginwindow starts and logs in (autologin)
#   xbox-gui.sh restart   stop, then start
#
# How Tiger behaves (2026-11-25): WindowServer is the on-demand mach_init service com.apple.windowserver
# (/etc/mach_init.d/WindowServer.plist), loginwindow comes from the console line in /etc/ttys. After a
# WindowServer exits, a new loginwindow does not get a new one: it either waits forever on the old port
# or aborts in a loop (launchd then pauses 30 s). Killing loginwindow with the console line on races
# with that respawn and was not reliable. With the console line off nothing respawns; WindowServer is
# started first (it waits until loginwindow connects), then the console line goes back on.
# Tested 2026-11-25 on main6: 5 restarts in a row, desktop back after 8.5-9.1 s each.
# Open applications are quit without saving. If the desktop does not come back: ssh xbox sudo reboot.
S="-o IgnoreUnknown=UseKeychain -o BatchMode=yes -o ConnectTimeout=10"
cmd=${1:-status}

remote=$(cat <<'EOF'
WS=/System/Library/Frameworks/ApplicationServices.framework/Frameworks/CoreGraphics.framework/Resources/WindowServer
LW='console[[:space:]]*"/System/Library/CoreServices/loginwindow.app/Contents/MacOS/loginwindow"[[:space:]]*vt100[[:space:]]*'
now() { perl -MTime::HiRes=time -e 'printf "%.1f", time'; }
since() { perl -MTime::HiRes=time -e "printf q{%.1f}, time-$1"; }
nap() { perl -e 'select(undef,undef,undef,0.5)'; }
pids() { ps -axww -o pid,command | awk '$2 ~ /\/CoreGraphics.framework\/Resources\/WindowServer$/ {w=$1} $2 ~ /\/loginwindow.app\/Contents\/MacOS\/loginwindow$/ {l=$1} $2 ~ /\/Finder.app\/Contents\/MacOS\/Finder$/ {f=$1} $2 ~ /\/Dock.app\/Contents\/MacOS\/Dock$/ {d=$1} END{print "ws=" w " lw=" l " finder=" f " dock=" d}'; }
field() { echo "$1" | sed -n "s/.*$2=\([0-9]*\).*/\1/p"; }
desktop() { p=$(pids); [ -n "$(field "$p" finder)" ] && [ -n "$(field "$p" dock)" ]; }
gui_down() { p=$(pids); [ -z "$(field "$p" ws)" ] && [ -z "$(field "$p" lw)" ]; }
stop() {
  [ -f /etc/ttys.osx360-gui.bak ] || sudo cp -p /etc/ttys /etc/ttys.osx360-gui.bak
  sudo sed -i '' "s|^\($LW\)on |\1off |" /etc/ttys
  grep -q "^${LW}off " /etc/ttys || { echo "FAIL ttys edit"; return 1; }
  t0=$(now); sudo kill -HUP 1
  i=0; while ! gui_down && [ $i -lt 60 ]; do nap; i=$((i+1)); done
  gui_down || { echo "FAIL GUI still up: $(pids)"; return 1; }
  echo "down after $(since $t0) s"
}
start() {
  t0=$(now)
  [ -n "$(field "$(pids)" ws)" ] || sudo sh -c "nohup $WS -daemon >/dev/null 2>&1 &"
  i=0; while [ $i -lt 4 ]; do nap; i=$((i+1)); done
  if [ -f /etc/ttys.osx360-gui.bak ]; then sudo cp -p /etc/ttys.osx360-gui.bak /etc/ttys && sudo rm /etc/ttys.osx360-gui.bak
  else sudo sed -i '' "s|^\($LW\)off |\1on |" /etc/ttys; fi
  grep -q "^${LW}on " /etc/ttys || { echo "FAIL ttys not restored"; return 1; }
  sudo kill -HUP 1
  i=0; while ! desktop && [ $i -lt 80 ]; do nap; i=$((i+1)); done
  if ! desktop; then
    # Second chance: the waiting loginwindow may need a WindowServer to answer it.
    [ -n "$(field "$(pids)" ws)" ] || sudo sh -c "nohup $WS -daemon >/dev/null 2>&1 &"
    i=0; while ! desktop && [ $i -lt 80 ]; do nap; i=$((i+1)); done
  fi
  desktop || { echo "FAIL no desktop: $(pids)"; return 1; }
  echo "desktop after $(since $t0) s: $(pids)"
}
case "$CMD" in
  status) pids ;;
  stop) stop ;;
  start) start ;;
  restart) echo "before $(pids)"; stop && start ;;
  *) echo "usage: status | stop | start | restart"; exit 2 ;;
esac
EOF
)
timeout 240 ssh $S xbox "CMD=$cmd; $remote"
