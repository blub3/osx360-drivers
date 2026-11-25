#!/bin/bash
# Stage 2 check (smp6a: all six hardware threads, PIR n = logical CPU n, started deferred, with
# XenonSMPIdleLow). Measures the same busy loop with 1, 3 (one thread per core) and 6 CPUs in one
# boot, then stops and starts every secondary once.
# Usage: smp6-test.sh   Log: $D/smp6-test.log
. "$(dirname "$0")/xbox-common.sh"
LOG=$D/smp6-test.log
: > "$LOG"

# Decrementer ticks over 5 s and external interrupts per second of logical CPU $1.
ticks() {
  local n=$1
  XT=60 x "rd() { sudo dd if=/dev/kmem bs=4 skip=\$(( \$1 / 4 )) count=1 2>/dev/null | od -An -tu4 | tr -d ' '; }
     p=\$(rd \$((0x365000 + $n * 16 + 0x0C))); a=\$(rd \$((p + 0x824))); e=\$(rd \$((p + 0x814))); sleep 5
     b=\$(rd \$((p + 0x824))); f=\$(rd \$((p + 0x814))); echo \"\$((b - a)) \$(( (f - e) / 5 ))\""
}

# The busy loop as a script on the Xbox: prints its own run time in seconds.
x 'cat > /tmp/osx360-test-w.pl' <<'EOF'
use Time::HiRes qw(time); my $s = time; my $x = 0; $x += $_ for 1..2500000; printf "%.2f", time - $s;
EOF

# loadN <k>: k loops in parallel; prints their times, slowest last.
loadN() {
  XT=300 x "for i in \$(jot $1); do perl /tmp/osx360-test-w.pl > /tmp/osx360-test-l-\$i.txt & done; wait
    for i in \$(jot $1); do cat /tmp/osx360-test-l-\$i.txt; echo; done | sort -n | tr '\n' ' '; rm -f /tmp/osx360-test-l-*.txt"
}
slowest() { echo "$*" | awk '{print $NF}'; }
le() { python3 -c "import sys; sys.exit(0 if float('$1') <= float('$2') else 1)"; }

cpuN() {  # cpuN <logical> stop|start
  if [ "$2" = stop ]; then XT=90 x "sudo ./xenon-cpu-start $1 stop" 2>&1 | tail -1; else XT=90 x "sudo ./xenon-cpu-start $1" 2>&1 | tail -1; fi
}
screensaver() { x 'ps -axco command | grep -c "^ScreenSaverEngine$"'; }

log "boot profile: $(x 'grep "^default=" /Volumes/OSX360BOOT/kboot.conf') uptime: $(x uptime)"
log "SMP boot lines: $(x 'grep -h "SMP enabled\|XenonSMP: release\|SMP:" /var/log/system.log | tail -4' | tr '\n' '|')"
nc=$(x 'sysctl -n hw.ncpu'); a=$(availcpu)
log "hw.ncpu=$nc hw.availcpu=$a $(desktop) screensaver=$(screensaver)"
[ "$nc" = 6 ] && [ "$a" = 1 ] || { fail "expected 6 CPUs registered and 1 running, got ncpu=$nc availcpu=$a"; log "SUMMARY failures=$(failures)"; exit 1; }
x 'grep -q "idle loop at low thread priority" /var/log/system.log' || fail "low-priority idle loop not installed"
panicked && fail "panic.log present after boot"

# 1 CPU: the other five threads wait at low priority.
l=$(loadN 1); log "1 CPU: one loop $l s"; one1=$l

# 3 CPUs, one thread per core.
for n in 2 4; do log "start CPU $n: $(cpuN $n start)"; sleep 2; done
a=$(availcpu); log "availcpu=$a"; [ "$a" = 3 ] || fail "availcpu=$a with CPUs 0, 2, 4"
l=$(loadN 1); log "3 CPUs: one loop $l s"; one3=$l
l=$(loadN 3); log "3 CPUs: three loops $l s"; three3=$(slowest $l)

# 6 CPUs.
for n in 1 3 5; do log "start CPU $n: $(cpuN $n start)"; sleep 2; done
a=$(availcpu); log "availcpu=$a"; [ "$a" = 6 ] || fail "availcpu=$a with all six"
for n in 1 2 3 4 5; do
  set -- $(ticks $n)
  log "CPU $n: ${1:-?} decrementer ticks in 5 s, ${2:-?} external interrupts/s"
  [ -n "$1" ] && [ "$1" -gt 0 ] || fail "CPU $n takes no decrementer ticks"
  [ -n "$2" ] && [ "$2" -lt 1000 ] || fail "CPU $n external interrupts ${2:-?}/s"
done
l=$(loadN 1); log "6 CPUs: one loop $l s"; one6=$l
l=$(loadN 3); log "6 CPUs: three loops $l s"; three6=$(slowest $l)
l=$(loadN 6); log "6 CPUs: six loops $l s"; six6=$(slowest $l)
log "screensaver=$(screensaver) clockspread=$(clockspread)s eth=$(ethcheck) $(desktop)"

# Criteria: one loop with six CPUs no slower than 4.70 s (stage 1b: 4.60 s); six loops finish in
# less than twice the time of three, i.e. more throughput with six CPUs than with three.
le "$one6" 4.70 || fail "one loop with 6 CPUs took $one6 s (limit 4.70)"
python3 -c "import sys; sys.exit(0 if float('$six6') < 2 * float('$three3') else 1)" || fail "six loops $six6 s, not faster per loop than three on 3 CPUs ($three3 s)"
log "throughput (loops per 10 s): 1 CPU $(python3 -c "print('%.2f' % (10/$one1))"), 3 CPUs $(python3 -c "print('%.2f' % (30/$three3))"), 6 CPUs $(python3 -c "print('%.2f' % (60/$six6))")"

# One stop/start round for each secondary.
for n in 5 4 3 2 1; do
  log "stop CPU $n: $(cpuN $n stop)"; sleep 3
  a=$(availcpu); set -- $(ticks $n)
  log "CPU $n stopped: availcpu=$a ticks_5s=${1:-?}"
  [ "$a" = 5 ] || fail "availcpu=$a after stopping CPU $n"
  [ "$1" = 0 ] || fail "CPU $n took ${1:-?} ticks while stopped"
  log "start CPU $n: $(cpuN $n start)"; sleep 3
  a=$(availcpu); set -- $(ticks $n); cs=$(clockspread)
  log "CPU $n running: availcpu=$a ticks_5s=${1:-?} clockspread=${cs}s"
  [ "$a" = 6 ] || fail "availcpu=$a after starting CPU $n"
  [ -n "$1" ] && [ "$1" -gt 0 ] || fail "CPU $n takes no ticks after start"
  [ "$cs" -ge 0 ] && [ "$cs" -le 2 ] || fail "clocks differ by $cs s"
done

l=$(loadN 6); log "after restarts: six loops $l s"
e=$(ethcheck); log "eth=$e $(desktop)"; [ "$e" = ok ] || fail "ethernet $e"
for r in 1 2 3; do
  t=$(XT=40 x "/usr/bin/time -p say \"Audio test after the restart.\" 2>&1 | awk '/^real/{print \$2}'")
  log "say run $r: ${t:-timeout} s"
  [ -n "$t" ] && python3 -c "import sys; sys.exit(0 if 2.99 <= float('$t') <= 4.99 else 1)" || fail "say run $r ${t:-timeout} s"
  sleep 3
done
x 'rm -f /tmp/osx360-test-w.pl'
panicked && fail "panic.log present"
log "SUMMARY failures=$(failures)"
