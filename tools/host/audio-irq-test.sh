#!/bin/bash
# Test of interrupt-driven audio (profile audirq). Log: $D/audio-irq-test.log
. "$(dirname "$0")/xbox-common.sh"
LOG=$D/audio-irq-test.log
: > "$LOG"
SENTENCE="Audio test after the restart."   # 3.99 s on the timer build (2026-11-25)

# External interrupts and decrementers per second on CPUs 0..2 over 5 s, as "e0 d0 e1 d1 e2 d2".
rates3() {
  XT=60 x 'rd() { sudo dd if=/dev/kmem bs=4 skip=$(( $1 / 4 )) count=1 2>/dev/null | od -An -tu4 | tr -d " "; }
    snap() { for n in 0 1 2; do p=$(rd $((0x365000 + n * 16 + 0x0C))); printf "%s %s " $(rd $((p + 0x814))) $(rd $((p + 0x824))); done; }
    a=$(snap); sleep 5; b=$(snap); set -- $a $b
    echo "$(( (${7}-${1})/5 )) $(( (${8}-${2})/5 )) $(( (${9}-${3})/5 )) $(( (${10}-${4})/5 )) $(( (${11}-${5})/5 )) $(( (${12}-${6})/5 ))"'
}

log "profile: $(x 'grep "^default=" /Volumes/OSX360BOOT/kboot.conf') uptime: $(x uptime) availcpu=$(availcpu) $(desktop)"
lines=$(x 'grep -h "XenonAudio" /var/log/system.log | grep -v Probe | tail -5')
log "XenonAudio lines: $(echo "$lines" | tr '\n' '|')"
echo "$lines" | grep -q "Interrupt-driven position updates enabled" || fail "interrupt mode not enabled at boot"
echo "$lines" | grep -q "storm" && fail "storm fallback in the log"
panicked && fail "panic.log present"

set -- $(rates3); idle_e0=$1; idle_d=$(( $2 + $4 + $6 ))
log "idle: cpu0 ext/s=$1 dec/s=$2 cpu1 ext/s=$3 dec/s=$4 cpu2 ext/s=$5 dec/s=$6"

# Long playback for the rate measurement.
x 'nohup sh -c "for i in 1 2 3 4; do say \"This is a long audio test sentence for the Xbox three sixty running Mac OS X Tiger, repeated to keep the audio engine busy.\"; done" > /dev/null 2>&1 &'
sleep 3
set -- $(rates3); play_e0=$1; play_d=$(( $2 + $4 + $6 ))
log "playing: cpu0 ext/s=$1 dec/s=$2 cpu1 ext/s=$3 dec/s=$4 cpu2 ext/s=$5 dec/s=$6 say=$(x 'ps -axco command | grep -c "^say$"')"
extra=$(( play_e0 - idle_e0 ))
[ "$extra" -ge 50 ] && [ "$extra" -le 250 ] || fail "CPU 0 extra external interrupts during playback: $extra/s (expected 50-250)"
[ "$play_d" -le $(( idle_d * 2 )) ] || fail "decrementer total during playback $play_d/s, more than twice idle ($idle_d/s)"
x 'killall say 2>/dev/null; true'

# Five short runs, each timed: exercises engine start and stop.
for r in 1 2 3 4 5; do
  t=$(XT=40 x "/usr/bin/time -p say \"$SENTENCE\" 2>&1 | awk '/^real/{print \$2}'")
  log "say run $r: ${t:-timeout} s"
  [ -n "$t" ] && python3 -c "import sys; t=float('$t'); sys.exit(0 if 2.99 <= t <= 4.99 else 1)" || fail "say run $r took ${t:-timeout} s (expected 3.0-5.0)"
  sleep 4
done

sleep 10
set -- $(rates3)
log "after: cpu0 ext/s=$1 dec/s=$2 cpu1 ext/s=$3 dec/s=$4 cpu2 ext/s=$5 dec/s=$6 $(desktop)"
[ "$1" -le $(( idle_e0 + 100 )) ] || fail "CPU 0 external interrupts after playback $1/s, idle was $idle_e0/s (storm?)"
lines=$(x 'grep -h "XenonAudio" /var/log/system.log | grep -v Probe | tail -3')
echo "$lines" | grep -q "storm" && fail "storm fallback during the test: $(echo "$lines" | grep storm)"
panicked && fail "panic.log present"
log "SUMMARY failures=$(failures)"
