#!/bin/bash
# Timer and CPU load on the Xbox, idle and while `say` plays speech. Log: $D/audio-measure.log
. "$(dirname "$0")/xbox-common.sh"
LOG=$D/audio-measure.log
: > "$LOG"
# Externals and decrementers of logical CPUs 0..2 plus top's CPU line, over 10 s.
sample() {
  XT=60 x 'rd() { sudo dd if=/dev/kmem bs=4 skip=$(( $1 / 4 )) count=1 2>/dev/null | od -An -tu4 | tr -d " "; }
    snap() { for n in 0 1 2; do p=$(rd $((0x365000 + n * 16 + 0x0C))); printf "%s %s " $(rd $((p + 0x814))) $(rd $((p + 0x824))); done; }
    a=$(snap); top -l 2 -s 10 -n 0 | grep "CPU usage" | tail -1 > /tmp/osx360-test-am-top.txt; b=$(snap)
    set -- $a $b
    echo "cpu0 ext/s=$(( (${7}-${1})/10 )) dec/s=$(( (${8}-${2})/10 )) cpu1 ext/s=$(( (${9}-${3})/10 )) dec/s=$(( (${10}-${4})/10 )) cpu2 ext/s=$(( (${11}-${5})/10 )) dec/s=$(( (${12}-${6})/10 )) | $(sed "s/^ *//" /tmp/osx360-test-am-top.txt)"; rm -f /tmp/osx360-test-am-top.txt'
}
log "uptime: $(x uptime) availcpu=$(availcpu)"
log "idle:    $(sample)"
x 'nohup sh -c "for i in 1 2 3 4 5 6; do say \"This is a long audio test sentence for the Xbox three sixty running Mac OS X Tiger, repeated to keep the engine busy.\"; done" > /tmp/osx360-test-say.log 2>&1 &' 
sleep 3
log "playing: $(sample) say_running=$(x 'ps -axco command | grep -c "^say$"')"
log "playing: $(sample) say_running=$(x 'ps -axco command | grep -c "^say$"')"
x 'killall say 2>/dev/null; cat /tmp/osx360-test-say.log; rm -f /tmp/osx360-test-say.log' >/dev/null 2>&1
sleep 5
log "after:   $(sample)"
