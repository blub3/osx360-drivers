#!/bin/bash
# One listening round: audio-round.sh <label> <ack> <posfix> <interrupt>. Log: $D/audio-rounds.log
. "$(dirname "$0")/xbox-common.sh"
LOG=$D/audio-rounds.log
label=$1; ack=$2; posfix=$3; irq=$4
log "== round $label: ack $ack posfix $posfix interrupt $irq"
for kv in "ack $ack" "posfix $posfix" "interrupt $irq"; do
  log "  $(x "sudo ./xenon-audio-set $kv" 2>&1 | tail -1)"
done
x 'sleep 30' >/dev/null
# Play a 7 s sentence three times; measure CPU 0 externals and decrementers in the middle.
x 'nohup sh -c "for i in 1 2 3; do say \"The quick brown fox jumps over the lazy dog, while the Xbox plays this sentence for the listening test.\"; sleep 1; done" > /dev/null 2>&1 &'
sleep 4
r=$(XT=40 x 'rd() { sudo dd if=/dev/kmem bs=4 skip=$(( $1 / 4 )) count=1 2>/dev/null | od -An -tu4 | tr -d " "; }
  snap() { t=0; for n in 0 1 2; do p=$(rd $((0x365000 + n * 16 + 0x0C))); t=$((t + $(rd $((p + 0x824))))); done; p=$(rd $((0x365000 + 0x0C))); echo $(rd $((p + 0x814))) $t; }
  a=$(snap); sleep 5; b=$(snap); set -- $a $b; echo "cpu0_ext/s=$(( ($3-$1)/5 )) dec_total/s=$(( ($4-$2)/5 ))"')
log "  during playback: $r"
log "  driver: $(x 'grep -h "XenonAudioDevice" /var/log/system.log | tail -1 | sed "s/^.*XenonAudioDevice:://"')"
x 'while ps -axco command | grep -q "^say$"; do sleep 1; done' >/dev/null
log "  round $label done"
