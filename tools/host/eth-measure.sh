#!/bin/bash
# Ethernet throughput with one and two CPUs in the same boot, unencrypted (nc) and over ssh.
# Usage: eth-measure.sh [cycle|now] [MiB] [runs]
#   cycle (default): measure with 2 CPUs, stop CPU 1, measure, start CPU 1, measure again.
#                    Needs the smppark build and the new xenon-cpu-start.
#   now:             measure the current state only.
# Xbox side: Hobbit nc 1.10 ("nc -l -p"). Linux side: OpenBSD nc. Log: $D/eth-measure.log
. "$(dirname "$0")/xbox-common.sh"
LOG=$D/eth-measure.log
MODE=${1:-cycle}
MIB=${2:-64}
RUNS=${3:-3}
BYTES=$((MIB * 1048576))
SSHMIB=16
PUP=5001; PDOWN=5002
: > "$LOG"

mibs() { python3 -c "import sys; print('%.2f' % ($1 / 1048576.0 / ($3 - $2)))"; }

cleanup_nc() { x 'killall nc 2>/dev/null; rm -f /tmp/osx360-test-nc-up.txt; true' >/dev/null 2>&1; }

# Linux -> Xbox, unencrypted. The Xbox counts the bytes with dd.
nc_up() {
  cleanup_nc
  timeout 120 ssh $S xbox "nc -l -p $PUP | dd of=/dev/null bs=65536 2> /tmp/osx360-test-nc-up.txt" &
  local srv=$! t0 t1 got
  sleep 2
  t0=$(now); head -c "$BYTES" /dev/zero | nc -N -w 30 "$XBOX_IP" "$PUP"; t1=$(now)
  wait $srv
  got=$(x 'sed -n "s/^\([0-9]*\) bytes.*/\1/p" /tmp/osx360-test-nc-up.txt')
  [ "$got" = "$BYTES" ] || { fail "nc up: Xbox received ${got:-nothing} of $BYTES bytes"; echo "nc_up=?"; return; }
  echo "nc_up=$(mibs $BYTES $t0 $t1)"
}

# Xbox -> Linux, unencrypted. Linux stops reading after BYTES, which ends the Xbox nc.
nc_down() {
  cleanup_nc
  timeout 120 ssh $S xbox "dd if=/dev/zero bs=65536 count=$((BYTES / 65536)) 2>/dev/null | nc -l -p $PDOWN" &
  local srv=$! t0 t1 got
  sleep 2
  t0=$(now); got=$(nc -d -w 30 "$XBOX_IP" "$PDOWN" | head -c "$BYTES" | wc -c); t1=$(now)
  sleep 1; cleanup_nc; wait $srv 2>/dev/null
  [ "$got" = "$BYTES" ] || { fail "nc down: received $got of $BYTES bytes"; echo "nc_down=?"; return; }
  echo "nc_down=$(mibs $BYTES $t0 $t1)"
}

# Same over ssh (encrypted), smaller.
ssh_up_down() {
  local b=$((SSHMIB * 1048576)) t0 t1 t2 got
  t0=$(now); head -c "$b" /dev/zero | x 'cat > /dev/null'; t1=$(now)
  got=$(x "dd if=/dev/zero bs=65536 count=$((b / 65536)) 2>/dev/null" | wc -c); t2=$(now)
  [ "$got" = "$b" ] || fail "ssh down: received $got of $b bytes"
  echo "ssh_up=$(mibs $b $t0 $t1) ssh_down=$(mibs $b $t1 $t2)"
}

# CPU usage on the Xbox during one more unencrypted upload and download (top samples 3 s).
cpu_during() {
  local dir=$1 out
  x "rm -f /tmp/osx360-test-top.txt; (sleep 3; top -l 2 -s 3 -n 0 > /tmp/osx360-test-top.txt) >/dev/null 2>&1 &" >/dev/null
  if [ "$dir" = up ]; then nc_up >/dev/null; else nc_down >/dev/null; fi
  sleep 1
  out=$(x 'grep "CPU usage" /tmp/osx360-test-top.txt | tail -1; rm -f /tmp/osx360-test-top.txt')
  echo "top_$dir=[$(echo "$out" | sed 's/^ *//')]"
}

measure() {
  local label=$1 a b t0 t1 r line
  log "== $label: availcpu=$(availcpu) $(desktop) screensaver=$(x 'ps -axco command | grep -c "^ScreenSaverEngine$"')"
  for r in $(seq 1 "$RUNS"); do
    a=$(counters); t0=$(date +%s)
    line="$(nc_up) $(nc_down)"
    b=$(counters); t1=$(date +%s)
    log "$label run $r: $line over $((t1 - t0)) s, $(rates "$a" "$b" $((t1 - t0)))"
  done
  log "$label: $(ssh_up_down)"
  log "$label: $(cpu_during up) $(cpu_during down)"
}

log "Xbox $XBOX_IP, $MIB MiB per transfer, $RUNS runs, mode $MODE, uptime: $(x uptime)"
case "$MODE" in
  now)
    measure "$(availcpu)cpu"
    ;;
  cycle)
    [ "$(availcpu)" = 2 ] || { log "ABORT: cycle mode needs two running CPUs"; exit 1; }
    [ "$(x 'md5 -q xenon-cpu-start' 2>/dev/null)" = "$TOOL_MD5" ] || { log "ABORT: new xenon-cpu-start not installed (run smppark-test.sh first)"; exit 1; }
    measure 2cpu-a
    log "stop CPU 1: $(cpu1 stop | tail -1)"; sleep 3
    [ "$(availcpu)" = 1 ] || { fail "CPU 1 did not stop"; log "SUMMARY failures=$(failures)"; exit 1; }
    measure 1cpu
    log "start CPU 1: $(cpu1 start | tail -1)"; sleep 3
    [ "$(availcpu)" = 2 ] || fail "CPU 1 did not start again"
    measure 2cpu-b
    ;;
  *) echo "usage: $0 [cycle|now] [MiB] [runs]" >&2; exit 2 ;;
esac
cleanup_nc
panicked && fail "panic.log present"
log "SUMMARY failures=$(failures)"
