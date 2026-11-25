#!/bin/bash
# Stage 1b check (smp3a: PIR 0, 2, 4 as logical CPUs 0, 1, 2).
# Usage: smp3-test.sh   Log: $D/smp3-test.log
. "$(dirname "$0")/xbox-common.sh"
LOG=$D/smp3-test.log
: > "$LOG"

# Decrementer ticks of logical CPU $1 over 5 s.
ticks() {
  local n=$1
  XT=60 x "rd() { sudo dd if=/dev/kmem bs=4 skip=\$(( \$1 / 4 )) count=1 2>/dev/null | od -An -tu4 | tr -d ' '; }
     p=\$(rd \$((0x365000 + $n * 16 + 0x0C))); a=\$(rd \$((p + 0x824))); e=\$(rd \$((p + 0x814))); sleep 5
     b=\$(rd \$((p + 0x824))); f=\$(rd \$((p + 0x814))); echo \"\$((b - a)) \$(( (f - e) / 5 ))\""
}

# One loop, then three loops in parallel.
load3() {
  XT=150 x 'w() { perl -MTime::HiRes=time -e "my \$s=time; my \$x=0; \$x+=\$_ for 1..2500000; printf \"%.2f\", time-\$s"; }
    one=$(w); (w > /tmp/osx360-test-l3-a.txt &); (w > /tmp/osx360-test-l3-b.txt &); c=$(w); sleep 4
    echo "one=$one three=$(cat /tmp/osx360-test-l3-a.txt)/$(cat /tmp/osx360-test-l3-b.txt)/$c screensaver=$(ps -axco command | grep -c "^ScreenSaverEngine$")"
    rm -f /tmp/osx360-test-l3-a.txt /tmp/osx360-test-l3-b.txt'
}

cpuN() {  # cpuN <logical> stop|start
  if [ "$2" = stop ]; then XT=90 x "sudo ./xenon-cpu-start $1 stop" 2>&1 | tail -1; else XT=90 x "sudo ./xenon-cpu-start $1" 2>&1 | tail -1; fi
}

log "boot profile: $(x 'grep "^default=" /Volumes/OSX360BOOT/kboot.conf') uptime: $(x uptime)"
log "SMP boot lines: $(x 'grep -h "SMP enabled\|released PIR\|SMP:" /var/log/system.log | tail -3' | tr '\n' '|')"
nc=$(x 'sysctl -n hw.ncpu'); a=$(availcpu)
log "hw.ncpu=$nc hw.availcpu=$a $(desktop) tool_state: $(x './xenon-cpu-start 9' 2>&1 | grep XenonCPU | tr '\n' '|')"
[ "$nc" = 3 ] && [ "$a" = 3 ] || { fail "expected 3 CPUs, got ncpu=$nc availcpu=$a"; log "SUMMARY failures=$(failures)"; exit 1; }
panicked && fail "panic.log present after boot"

for n in 1 2; do
  set -- $(ticks $n)
  log "CPU $n: ${1:-?} decrementer ticks in 5 s, ${2:-?} external interrupts/s"
  [ -n "$1" ] && [ "$1" -gt 0 ] || fail "CPU $n takes no decrementer ticks"
  [ -n "$2" ] && [ "$2" -lt 1000 ] || fail "CPU $n external interrupts ${2:-?}/s"
done
log "load: $(load3) clockspread=$(clockspread)s eth=$(ethcheck)"

# One stop/start round for each secondary.
for n in 2 1; do
  log "stop CPU $n: $(cpuN $n stop)"; sleep 3
  a=$(availcpu); set -- $(ticks $n)
  log "CPU $n stopped: availcpu=$a ticks_5s=${1:-?} $(desktop)"
  [ "$a" = 2 ] || fail "availcpu=$a after stopping CPU $n"
  [ "$1" = 0 ] || fail "CPU $n took ${1:-?} ticks while stopped"
  log "start CPU $n: $(cpuN $n start)"; sleep 3
  a=$(availcpu); set -- $(ticks $n); cs=$(clockspread)
  log "CPU $n running: availcpu=$a ticks_5s=${1:-?} clockspread=${cs}s eth=$(ethcheck)"
  [ "$a" = 3 ] || fail "availcpu=$a after starting CPU $n"
  [ -n "$1" ] && [ "$1" -gt 0 ] || fail "CPU $n takes no ticks after start"
  [ "$cs" -ge 0 ] && [ "$cs" -le 2 ] || fail "clocks differ by $cs s"
done
log "load after restarts: $(load3)"

panicked && fail "panic.log present"
log "SUMMARY failures=$(failures)"
