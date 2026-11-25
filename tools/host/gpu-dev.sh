#!/bin/bash
# Development loop for hand-loaded kexts (GPU plan step 0.6, docs/gpu-driver-plan.md): build on the G5,
# check the kernel imports, install on the Xbox outside /System/Library/Extensions, load, collect the
# kernel log, unload. No reboot; nothing here is ever loaded at boot.
#
#   tools/host/gpu-dev.sh KEXT [COMMAND] [OPTIONS]
#
#   KEXT        kext folder in this repository, for example XenosGPUProbe
#   COMMAND     build    sync the tree to the G5, build, check imports
#               install  build, then copy to /Library/OSX360Dev on the Xbox (root:wheel)
#               load     load the installed kext, wait, show its log lines
#               unload   unload it and show its log lines
#               cycle    install, load, wait, unload, show the log (default)
#               log      show the kernel lines with KEXT in them since the last load by this script
#   OPTIONS     --key NAME     set personality key NAME to true for this load (repeatable)
#               --wait SECONDS time between load and unload or log (default 5)
#               --gui-down     stop the GUI (tools/host/xbox-gui.sh) around unload, for kexts with user clients
#               --strict       fail on kernel imports no main6 boot kext uses (for kexts headed into the mkext)
#
# Import check: imports starting with ___ are compiler helpers (__udivdi3, __divdi3, shifts, float
# conversions); the Tiger kernel exports none of them, and kextload -n -t does not catch it, so they
# always fail. Imports outside tools/gpu/main6-boot-imports.txt are listed; --strict makes them fatal.
set -e
S="-o IgnoreUnknown=UseKeychain -o BatchMode=yes -o ConnectTimeout=10"
HERE=$(cd "$(dirname "$0")/../.." && pwd)
KEXT=${1:?usage: gpu-dev.sh KEXT [build|install|load|unload|cycle|log] [--key NAME] [--wait S] [--gui-down] [--strict]}
shift
CMD=cycle
case "$1" in build|install|load|unload|cycle|log) CMD=$1; shift;; esac
KEYS=(); WAIT=5; GUI=0; STRICT=0
while [ $# -gt 0 ]; do
  case "$1" in
    --key) KEYS+=("$2"); shift 2;;
    --wait) WAIT=$2; shift 2;;
    --gui-down) GUI=1; shift;;
    --strict) STRICT=1; shift;;
    *) echo "unknown option $1"; exit 2;;
  esac
done
[ -d "$HERE/$KEXT" ] || { echo "no kext folder $HERE/$KEXT"; exit 2; }
BUNDLE=fish.goldfish64.osx360.$KEXT
BUILD_DIR=osx360-build/gpu-build
DEV_DIR=/Library/OSX360Dev
MARK=/tmp/osx360-gpu-dev-$KEXT.mark

build() {
  echo "== build $KEXT on the G5"
  rsync -a --delete --exclude .git --exclude 'build_*' --exclude 'gpu/lab' --exclude MacPPCKernelSDK \
    -e "ssh $S" "$HERE/" "g5:$BUILD_DIR/"
  timeout 900 ssh $S g5 "cd ~/$BUILD_DIR && ln -sfn ~/osx360-build/osx360-drivers/MacPPCKernelSDK MacPPCKernelSDK &&
    make -C $KEXT OSX_VERSION=tiger DARLING_SHELL= AS=/usr/bin/as CC=\$HOME/osx360-build/bin/g5-cc \
      CXX=/usr/bin/powerpc-apple-darwin9-g++-4.2.1 LD=/usr/bin/ld_classic > /tmp/osx360-gpu-dev-build.log 2>&1 \
      || { tail -30 /tmp/osx360-gpu-dev-build.log; exit 1; }
    grep -i -E 'warning|error' /tmp/osx360-gpu-dev-build.log | head -20
    B=$KEXT/build_kext_tiger/$KEXT.kext/Contents/MacOS/$KEXT
    echo \"binary \$(md5 -q \$B) \$(stat -f %z \$B) bytes\"
    nm -u \$B | sort -u > /tmp/osx360-gpu-dev-imports.txt
    helpers=\$(grep '^___' /tmp/osx360-gpu-dev-imports.txt | tr '\n' ' ')
    other=\$(comm -23 /tmp/osx360-gpu-dev-imports.txt tools/gpu/main6-boot-imports.txt | tr '\n' ' ')
    [ -z \"\$helpers\" ] || { echo \"FAIL compiler helper imports the kernel does not export: \$helpers\"; exit 1; }
    if [ -n \"\$other\" ]; then
      echo \"imports no main6 boot kext uses: \$other\"
      [ $STRICT = 0 ] || { echo 'FAIL --strict'; exit 1; }
    else
      echo 'imports: all used by main6 boot kexts'
    fi"
}

install() {
  echo "== install $KEXT on the Xbox"
  timeout 120 ssh $S g5 "cd ~/$BUILD_DIR/$KEXT/build_kext_tiger && tar cf - $KEXT.kext" |
    timeout 120 ssh $S xbox "sudo mkdir -p $DEV_DIR && cd $DEV_DIR && sudo rm -rf $KEXT.kext && sudo tar xmf - &&
      sudo chown -R root:wheel $KEXT.kext && sudo chmod -R go-w $KEXT.kext &&
      echo \"installed \$(md5 -q $KEXT.kext/Contents/MacOS/$KEXT)\""
}

load() {
  echo "== load $KEXT${KEYS[*]:+ with ${KEYS[*]}}"
  insert=""
  for key in "${KEYS[@]}"; do insert="$insert<key>$key</key>\\n\\t\\t\\t<true/>\\n\\t\\t\\t"; done
  timeout 120 ssh $S xbox "P=$DEV_DIR/$KEXT.kext/Contents/Info.plist
    wc -l < /var/log/system.log | tr -d ' ' > $MARK
    if [ -n '$insert' ]; then
      sudo cp -p \$P /tmp/osx360-gpu-dev-Info.plist
      sudo perl -0pi -e 's|(<key>IOMatchCategory</key>)|$insert\$1|' \$P
    fi
    sudo kextload $DEV_DIR/$KEXT.kext 2>&1 | tail -3; status=\$?
    if [ -n '$insert' ]; then sudo cp -p /tmp/osx360-gpu-dev-Info.plist \$P; fi
    kextstat | grep -q $BUNDLE && echo 'loaded' || echo 'NOT loaded'"
}

unload() {
  [ $GUI = 0 ] || "$HERE/tools/host/xbox-gui.sh" stop
  echo "== unload $KEXT"
  timeout 120 ssh $S xbox "for i in 1 2 3 4 5 6 7 8 9 10; do
      sudo kextunload -b $BUNDLE > /tmp/osx360-gpu-dev-unload.txt 2>&1 && break
      perl -e 'select(undef,undef,undef,2)'
    done; tail -1 /tmp/osx360-gpu-dev-unload.txt
    kextstat | grep -q $BUNDLE && echo 'STILL loaded' || echo 'unloaded'"
  [ $GUI = 0 ] || "$HERE/tools/host/xbox-gui.sh" start
}

log() {
  echo "== kernel log for $KEXT"
  timeout 60 ssh $S xbox "from=\$(cat $MARK 2>/dev/null || echo 0)
    tail -n +\$((from + 1)) /var/log/system.log | grep -E 'kernel\\[0\\]:.*($KEXT|panic|kld|kmod)' | cut -c1-15,40-260
    ls /Library/Logs/panic.log 2>/dev/null && echo 'PANIC LOG PRESENT' || true"
}

pause() { timeout $((WAIT + 30)) ssh $S xbox "perl -e 'select(undef,undef,undef,$WAIT)'"; }

case $CMD in
  build) build ;;
  install) build; install ;;
  load) load; pause; log ;;
  unload) unload; log ;;
  cycle) build; install; load; pause; unload; log ;;
  log) log ;;
esac
