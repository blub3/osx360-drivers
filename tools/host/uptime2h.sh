#!/bin/bash
# Two-hour SMP uptime test for the Xbox (smp1c). Heartbeat every 60 s, checks every 10 minutes.
D=${XBOX_TEST_OUT:-$HOME/xbox-test-logs}; mkdir -p "$D"
LOG=$D/uptime2h.log
S="-o IgnoreUnknown=UseKeychain -o BatchMode=yes -o ConnectTimeout=10 -o ServerAliveInterval=5 -o ServerAliveCountMax=4"
DURATION=${DURATION:-7200}
start=$(date +%s)
miss=0; maxmiss=0; beats=0; fails=0
: > "$LOG"
head -c 10485760 /dev/urandom > "$D/uptime-eth.bin"
L=$(md5sum < "$D/uptime-eth.bin" | cut -d' ' -f1)

log() { echo "$(date +%H:%M:%S) $*" >> "$LOG"; }

check() {
  # CPU 1 interrupt and timer counters (hwCtr at per_proc + 0x800), load test, Ethernet.
  out=$(timeout 120 ssh $S xbox '
    v1=$(sudo dd if=/dev/kmem bs=4 skip=$((0x365000/4 + 7)) count=1 2>/dev/null | od -An -tu4 | tr -d " ")
    rd() { sudo dd if=/dev/kmem bs=4 skip=$(( $1 / 4 )) count=1 2>/dev/null | od -An -tu4 | tr -d " "; }
    e=$(rd $((v1+0x814))); d=$(rd $((v1+0x824))); sleep 5
    e2=$(rd $((v1+0x814))); d2=$(rd $((v1+0x824)))
    w() { perl -MTime::HiRes=time -e "my \$s=time; my \$x=0; \$x+=\$_ for 1..2500000; printf \"%.2f\", time-\$s"; }
    one=$(w); (w > /tmp/osx360-test-ut-a.txt &); two=$(w); sleep 2
    echo "cpu1_ext_per_s=$(( (e2-e)/5 )) cpu1_dec_per_s=$(( (d2-d)/5 )) one=$one two=$(cat /tmp/osx360-test-ut-a.txt)/$two"
    rm -f /tmp/osx360-test-ut-a.txt
    ls /Library/Logs/panic.log >/dev/null 2>&1 && echo PANICLOG' 2>&1)
  log "CHECK $out"
  up=$(timeout 90 ssh $S xbox 'cat > /tmp/osx360-test-ut.bin; md5 -q /tmp/osx360-test-ut.bin' < "$D/uptime-eth.bin" 2>/dev/null)
  down=$(timeout 90 ssh $S xbox 'cat /tmp/osx360-test-ut.bin; rm -f /tmp/osx360-test-ut.bin' 2>/dev/null | md5sum | cut -d' ' -f1)
  if [ "$up" = "$L" ] && [ "$down" = "$L" ]; then log "ETH ok"; else log "ETH FAIL up=$up down=$down"; fails=$((fails+1)); fi
  case "$out" in *PANICLOG*) fails=$((fails+1));; esac
  ext=$(echo "$out" | sed -n 's/.*cpu1_ext_per_s=\([0-9]*\).*/\1/p')
  if [ -z "$ext" ] || [ "$ext" -ge 1000 ]; then log "CPU1 FAIL ext=$ext"; fails=$((fails+1)); fi
}

next_check=0
while [ $(( $(date +%s) - start )) -lt "$DURATION" ]; do
  hb=$(timeout 30 ssh $S xbox 'echo "up=$(uptime | sed "s/^ *//") avail=$(sysctl -n hw.availcpu) ws=$(ps -axco command | grep -c "^WindowServer$") finder=$(ps -axco command | grep -c "^Finder$") dock=$(ps -axco command | grep -c "^Dock$")"' 2>/dev/null)
  beats=$((beats+1))
  if [ -z "$hb" ]; then
    miss=$((miss+1)); [ $miss -gt $maxmiss ] && maxmiss=$miss; log "HB MISS ($miss in a row)"
    if [ $miss -ge 10 ]; then log "ABORT: Xbox unreachable for 10 heartbeats"; break; fi
  else
    miss=0; log "HB $hb"
    case "$hb" in *"avail=2 ws=1 finder=1 dock=1"*) ;; *) log "HB STATE FAIL"; fails=$((fails+1));; esac
  fi
  if [ $(( $(date +%s) - start )) -ge $next_check ] && [ $miss -eq 0 ]; then
    check; next_check=$(( $(date +%s) - start + 600 ))
  fi
  sleep 60
done
[ $miss -eq 0 ] && check
rm -f "$D/uptime-eth.bin"
log "SUMMARY elapsed=$(( $(date +%s) - start ))s heartbeats=$beats max_missed_in_a_row=$maxmiss failures=$fails"
tail -1 "$LOG"
