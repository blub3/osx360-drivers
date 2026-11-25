#!/bin/bash
# Stop/start test for the smppark build: CPU 1 is taken offline with IOCPUState = "stopped"
# (haltCPU -> processor_exit -> quiesceCPU parks PIR 2) and started again, ROUNDS times.
# Usage: smppark-test.sh [rounds]   (default 3). Log: $D/smppark-test.log
. "$(dirname "$0")/xbox-common.sh"
LOG=$D/smppark-test.log
ROUNDS=${1:-3}
: > "$LOG"

# --- Preconditions: right profile, two CPUs, desktop up, no panic log.
log "boot profile: $(x 'grep "^default=" /Volumes/OSX360BOOT/kboot.conf') uptime: $(x uptime)"
x 'grep -qE "^default=(smppark|smppk2)$" /Volumes/OSX360BOOT/kboot.conf' || { log "ABORT: default boot profile is not smppark or smppk2"; exit 1; }
[ "$(availcpu)" = 2 ] || { log "ABORT: hw.availcpu is not 2 after boot"; exit 1; }
log "desktop: $(desktop)"
panicked && { log "ABORT: /Library/Logs/panic.log exists before the test"; exit 1; }
log "SMP boot lines: $(x 'grep -h "XenonSMP\|SMP enabled\|SMP:\|PerProcTable" /var/log/system.log | tail -4' | tr '\n' '|')"

# --- Install the start/stop tool (keeps the previous one as xenon-cpu-start.old).
if [ "$(x 'md5 -q xenon-cpu-start' 2>/dev/null)" != "$TOOL_MD5" ]; then
  ssh $S g5 'cat ~/osx360-build/xenon-cpu-start.new' | x "cat > xenon-cpu-start.tmp; test \"\$(md5 -q xenon-cpu-start.tmp)\" = $TOOL_MD5 && chmod 755 xenon-cpu-start.tmp && { test -e xenon-cpu-start.old || cp -p xenon-cpu-start xenon-cpu-start.old; } && mv -f xenon-cpu-start.tmp xenon-cpu-start" \
    || { log "ABORT: tool install failed"; exit 1; }
fi
log "tool: $(x 'md5 -q xenon-cpu-start') state: $(x './xenon-cpu-start 9' 2>&1 | grep XenonCPU | tr '\n' '|')"

log "baseline 2 CPUs: $(loadcheck) cpu1_ticks_5s=$(cpu1_ticks) eth=$(ethcheck) clockspread=$(clockspread)"

for r in $(seq 1 "$ROUNDS"); do
  # Stop.
  t0=$(now); out=$(cpu1 stop); t1=$(now)
  log "round $r stop: $(echo "$out" | tail -1) ($(secs $t0 $t1) s)"
  sleep 3
  a=$(availcpu); ticks=$(cpu1_ticks)
  log "round $r stopped: availcpu=$a cpu1_ticks_5s=$ticks $(desktop) $(loadcheck)"
  [ "$a" = 1 ] || fail "round $r: hw.availcpu=$a after stop, expected 1"
  [ "$ticks" = 0 ] || fail "round $r: CPU 1 took $ticks decrementer ticks while stopped"
  if [ -z "$a" ]; then log "ABORT: Xbox not answering after stop"; break; fi

  # Start.
  t0=$(now); out=$(cpu1 start); t1=$(now)
  log "round $r start: $(echo "$out" | tail -1) ($(secs $t0 $t1) s)"
  sleep 3
  a=$(availcpu); ticks=$(cpu1_ticks)
  cs=$(clockspread)
  log "round $r running: availcpu=$a cpu1_ticks_5s=$ticks clockspread=${cs}s $(desktop) $(loadcheck) eth=$(ethcheck)"
  [ "$cs" -ge 0 ] && [ "$cs" -le 2 ] || fail "round $r: clocks differ by $cs s across processes"
  [ "$a" = 2 ] || fail "round $r: hw.availcpu=$a after start, expected 2"
  [ -n "$ticks" ] && [ "$ticks" -gt 0 ] || fail "round $r: CPU 1 takes no decrementer ticks after start"
  if [ -z "$a" ]; then log "ABORT: Xbox not answering after start"; break; fi
done

# --- Afterwards: panic log, kernel messages from the stop/start paths.
panicked && fail "panic.log present"
log "kernel lines: $(x 'grep -h "Stopping CPU\|processor_exit returned\|Starting deferred\|processor_start returned\|released PIR\|zombie\|XenonSMP" /var/log/system.log | tail -12' | tr '\n' '|')"
log "SUMMARY rounds=$ROUNDS failures=$(failures)"
