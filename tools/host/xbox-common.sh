# Shared helpers for the Xbox SMP test scripts. Source, do not run.
D=${XBOX_TEST_OUT:-$HOME/xbox-test-logs}; mkdir -p "$D"
S="-o IgnoreUnknown=UseKeychain -o BatchMode=yes -o ConnectTimeout=10 -o ServerAliveInterval=5 -o ServerAliveCountMax=4"
XBOX_IP=$(ssh -G -o IgnoreUnknown=UseKeychain xbox | awk '$1=="hostname"{print $2}')
TOOL_MD5=d085ca0b62ac4f27b9f42983871f23d2   # tools/smp/xenon-cpu-start.c with "stop", built on the G5

# log goes to the file and stderr, so it also works inside $(...).
log() { echo "$(date +%H:%M:%S) $*" | tee -a "$LOG" >&2; }
fail() { log "FAIL $*"; }
failures() { grep -c " FAIL " "$LOG"; }
secs() { python3 -c "import sys; print('%.1f' % ($2 - $1))"; }
x() { timeout "${XT:-60}" ssh $S xbox "$@"; }
now() { date +%s.%N; }

if pgrep -f 'uptime(2h|3)\.sh' >/dev/null; then
  echo "an uptime test is still running; not starting" >&2
  exit 3
fi

# Interrupt counters from per_proc hwCtr (+0x814 externals, +0x824 decrementers) of logical
# CPUs 0 and 1. PerProcTable (0x365000) entries are 16 bytes, per_proc address at +0x0C.
# Prints "ext0 dec0 ext1 dec1".
counters() {
  x 'rd() { sudo dd if=/dev/kmem bs=4 skip=$(( $1 / 4 )) count=1 2>/dev/null | od -An -tu4 | tr -d " "; }
     p0=$(rd $((0x365000 + 0x0C))); p1=$(rd $((0x365000 + 16 + 0x0C)))
     echo "$(rd $((p0+0x814))) $(rd $((p0+0x824))) $(rd $((p1+0x814))) $(rd $((p1+0x824)))"'
}

# Per-second rates between two counters() outputs over $3 seconds.
rates() {
  local a=($1) b=($2) s=$3
  [ "${#a[@]}" -eq 4 ] && [ "${#b[@]}" -eq 4 ] && [ "$s" -gt 0 ] || { echo "rates=unavailable"; return; }
  echo "cpu0_ext/s=$(( (b[0]-a[0]) / s )) cpu0_dec/s=$(( (b[1]-a[1]) / s )) cpu1_ext/s=$(( (b[2]-a[2]) / s )) cpu1_dec/s=$(( (b[3]-a[3]) / s ))"
}

# Logical CPU 1 decrementer ticks over 5 seconds (0 = parked).
cpu1_ticks() {
  local a b
  a=$(counters) && sleep 5 && b=$(counters) || { echo -1; return; }
  set -- $a; local d1=$4; set -- $b; echo $(( $4 - d1 ))
}

availcpu() { x 'sysctl -n hw.availcpu' 2>/dev/null; }

# start or stop logical CPU 1 with the new tool; prints the tool output.
cpu1() {
  if [ "$1" = stop ]; then XT=90 x 'sudo ./xenon-cpu-start 1 stop' 2>&1; else XT=90 x 'sudo ./xenon-cpu-start 1' 2>&1; fi
}

# One and two parallel busy loops (seconds each), for information.
loadcheck() {
  XT=120 x 'w() { perl -MTime::HiRes=time -e "my \$s=time; my \$x=0; \$x+=\$_ for 1..2500000; printf \"%.2f\", time-\$s"; }
    one=$(w); (w > /tmp/osx360-test-lc-a.txt &); two=$(w); sleep 3
    ss=$(ps -axco command | grep -c "^ScreenSaverEngine$")
    echo "one=$one two=$(cat /tmp/osx360-test-lc-a.txt)/$two screensaver=$ss"; rm -f /tmp/osx360-test-lc-a.txt'
}

# 10 MiB up and down over ssh with checksums. Prints ok or the checksums.
ethcheck() {
  local f=$D/eth-check.bin L up down
  [ -f "$f" ] || head -c 10485760 /dev/urandom > "$f"
  L=$(md5sum < "$f" | cut -d' ' -f1)
  up=$(XT=90 x 'cat > /tmp/osx360-test-ec.bin; md5 -q /tmp/osx360-test-ec.bin' < "$f" 2>/dev/null)
  down=$(XT=90 x 'cat /tmp/osx360-test-ec.bin; rm -f /tmp/osx360-test-ec.bin' 2>/dev/null | md5sum | cut -d' ' -f1)
  if [ "$up" = "$L" ] && [ "$down" = "$L" ]; then echo ok; else echo "up=$up down=$down expected=$L"; fi
}

desktop() {
  x 'echo "ws=$(ps -axco command | grep -c "^WindowServer$") finder=$(ps -axco command | grep -c "^Finder$") dock=$(ps -axco command | grep -c "^Dock$")"' 2>/dev/null
}

panicked() { x 'ls /Library/Logs/panic.log' >/dev/null 2>&1; }

# Spread of `date +%s` over 60 short processes (they land on both CPUs). 0-1 is normal.
clockspread() {
  x 'for i in $(jot 60); do date +%s; done' 2>/dev/null | sort -n | awk 'NR==1{a=$1} {b=$1} END{if (NR==60) print b-a; else print -1}'
}
