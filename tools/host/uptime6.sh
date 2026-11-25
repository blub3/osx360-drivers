#!/bin/bash
# Two-hour SMP uptime test for the Xbox, six CPUs (stage 2, profile smp6c). Starts deferred CPUs
# 1-5 first if needed. Heartbeat every 60 s, checks every 10 minutes.
D=${XBOX_TEST_OUT:-$HOME/xbox-test-logs}; mkdir -p "$D"
LOG=$D/uptime6.log
S="-o IgnoreUnknown=UseKeychain -o BatchMode=yes -o ConnectTimeout=10 -o ServerAliveInterval=5 -o ServerAliveCountMax=4"
DURATION=${DURATION:-7200}
miss=0; maxmiss=0; beats=0; fails=0
: > "$LOG"
head -c 10485760 /dev/urandom > "$D/uptime6-eth.bin"
L=$(md5sum < "$D/uptime6-eth.bin" | cut -d' ' -f1)

log() { echo "$(date +%H:%M:%S) $*" >> "$LOG"; }

# Start the deferred CPUs.
for n in 1 2 3 4 5; do
  if [ "$(timeout 30 ssh $S xbox 'sysctl -n hw.availcpu' 2>/dev/null)" != 6 ]; then
    log "START CPU $n: $(timeout 90 ssh $S xbox "sudo ./xenon-cpu-start $n" 2>&1 | grep -i 'IOCPUState')"
    sleep 2
  fi
done
log "PROFILE $(timeout 30 ssh $S xbox 'grep "^default=" /Volumes/OSX360BOOT/kboot.conf; sysctl -n hw.ncpu hw.availcpu' 2>/dev/null | tr '\n' ' ')"
start=$(date +%s)

check() {
  # Interrupt and timer counters of CPUs 1-5 (hwCtr at per_proc + 0x800) over 5 s, then one loop
  # and six loops in parallel, then Ethernet.
  out=$(timeout 200 ssh $S xbox '
    rd() { sudo dd if=/dev/kmem bs=4 skip=$(( $1 / 4 )) count=1 2>/dev/null | od -An -tu4 | tr -d " "; }
    snap() { for n in 1 2 3 4 5; do p=$(rd $((0x365000 + n * 16 + 0x0C))); printf "%s %s " $(rd $((p+0x814))) $(rd $((p+0x824))); done; }
    a=$(snap); sleep 5; b=$(snap); set -- $a $b
    printf "cpu1_ext_per_s=%d cpu1_dec_per_s=%d cpu2_ext_per_s=%d cpu2_dec_per_s=%d cpu3_ext_per_s=%d cpu3_dec_per_s=%d " $(( (${11}-$1)/5 )) $(( (${12}-$2)/5 )) $(( (${13}-$3)/5 )) $(( (${14}-$4)/5 )) $(( (${15}-$5)/5 )) $(( (${16}-$6)/5 ))
    printf "cpu4_ext_per_s=%d cpu4_dec_per_s=%d cpu5_ext_per_s=%d cpu5_dec_per_s=%d " $(( (${17}-$7)/5 )) $(( (${18}-$8)/5 )) $(( (${19}-$9)/5 )) $(( (${20}-${10})/5 ))
    w() { perl -MTime::HiRes=time -e "my \$s=time; my \$x=0; \$x+=\$_ for 1..2500000; printf q{%.2f}, time-\$s"; }
    one=$(w)
    for i in 1 2 3 4 5 6; do w > /tmp/osx360-test-u6-$i.txt & done; wait
    six=$(for i in 1 2 3 4 5 6; do cat /tmp/osx360-test-u6-$i.txt; echo; done | sort -n | tr "\n" "/")
    ss=$(ps -axco command | grep -c "^ScreenSaverEngine$")
    echo "one=$one six=$six screensaver=$ss"
    rm -f /tmp/osx360-test-u6-*.txt
    ls /Library/Logs/panic.log >/dev/null 2>&1 && echo PANICLOG' 2>&1)
  log "CHECK $out"
  up=$(timeout 90 ssh $S xbox 'cat > /tmp/osx360-test-u6.bin; md5 -q /tmp/osx360-test-u6.bin' < "$D/uptime6-eth.bin" 2>/dev/null)
  down=$(timeout 90 ssh $S xbox 'cat /tmp/osx360-test-u6.bin; rm -f /tmp/osx360-test-u6.bin' 2>/dev/null | md5sum | cut -d' ' -f1)
  if [ "$up" = "$L" ] && [ "$down" = "$L" ]; then log "ETH ok"; else log "ETH FAIL up=$up down=$down"; fails=$((fails+1)); fi
  case "$out" in *PANICLOG*) fails=$((fails+1));; esac
  case "$out" in *"one= "*|*"six=/"*|*"six= "*) log "LOAD FAIL (no timing)"; fails=$((fails+1));; esac
  for c in 1 2 3 4 5; do
    ext=$(echo "$out" | sed -n "s/.*cpu${c}_ext_per_s=\([0-9]*\).*/\1/p")
    dec=$(echo "$out" | sed -n "s/.*cpu${c}_dec_per_s=\([0-9]*\).*/\1/p")
    if [ -z "$ext" ] || [ "$ext" -ge 1000 ]; then log "CPU$c FAIL ext=$ext"; fails=$((fails+1)); fi
    if [ -z "$dec" ] || [ "$dec" -le 0 ]; then log "CPU$c FAIL no decrementer ticks dec=$dec"; fails=$((fails+1)); fi
  done
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
    case "$hb" in *"avail=6 ws=1 finder=1 dock=1"*) ;; *) log "HB STATE FAIL"; fails=$((fails+1));; esac
  fi
  if [ $(( $(date +%s) - start )) -ge $next_check ] && [ $miss -eq 0 ]; then
    check; next_check=$(( $(date +%s) - start + 600 ))
  fi
  sleep 60
done
[ $miss -eq 0 ] && check
rm -f "$D/uptime6-eth.bin"
log "SUMMARY elapsed=$(( $(date +%s) - start ))s heartbeats=$beats max_missed_in_a_row=$maxmiss failures=$fails"
tail -1 "$LOG"
