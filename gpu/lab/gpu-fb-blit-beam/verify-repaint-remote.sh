set -eu
export LC_ALL=C
boot_volume=/Volumes/OSX360BOOT
/sbin/mount | grep -Fq " on $boot_volume ("
cd "$boot_volume"
check() {
    if test "$(md5 -q "$2")" != "$1"; then
        echo "Checksum mismatch for $2" >&2
        exit 1
    fi
}
check 92b5d269d906ca20b85264513d97e5b6 kboot.conf
check c76086865a738b203ebe3aab6f628dd5 kboot.conf.before-gpufbblitbeam.bak
check c76086865a738b203ebe3aab6f628dd5 kboot.conf.before-gpufbblit.bak
check d4e5729d0e58cd7b12194d3469f59509 kboot.conf.before-gpucompbootfix.bak
check 3d5be96c3df8c4c3cdfc514c7c637ba1 CMPFIX.ELF
check c603562c9452658c09f5e25db66ff936 XWCF.MKE
check b99d4be9377e3e7965bb3e88d5f1ff0e NativeHD.mkext
check ca72c036347285e3fe77b58d3b2c93c6 openbios-xbox360-native-hdmi-test.elf
check 2ce8cf67dd57d0ef8b68beaa342e051a openbios-xbox360.720p-hdmi.elf.bak
check a1f4dee1cea39d416ec981989d254556 XWBL.MKE
check 06b186cf2937bd8a03af7f43c525ed6e BLITSURF.ELF
check 320b36a69712ad437d044017d633d45c XWBM.MKE
check e5e98da3e48d8d78b04a2195481b0237 BLITBEAM.ELF
boot=$(/usr/sbin/sysctl -n kern.boottime)
printf 'boot=%s\n' "$boot"
windowserver() {
    ps -axww -o pid,command | awk '$2 ~ /\/WindowServer$/ && $3 == "-daemon" { print $1 }'
}
windowserver_pid=$(windowserver)
case "$windowserver_pid" in ''|*[!0-9]*) exit 1 ;; esac
/usr/sbin/kextstat | grep -E 'fish.goldfish64.osx360.Xenon(FramebufferTest|CoreGraphics)'
/usr/sbin/ioreg -w0 -c XenonFramebufferTest |
    grep -E 'IOCFPlugInTypes|IOAccelRevision|IOGLBundleName|XenosWindowServerFramebufferPID'
cd XenosFramebufferBeamTest
check d3a5d33e74cf4d8a23051fcd76ab35ff RunXenosFramebufferBlit.sh
check 7b49f2e9335352fc07dd9c9414e4f5c8 XenosFramebufferBlitCheck
check c86a8816a2dad8b199cecee938df8d11 XenosFramebufferRepaintCheck
scratch=$(mktemp -d /tmp/xenos-beam-verify.XXXXXXXX)
trap 'rm -rf "$scratch"' 0
stamp=$(date -r "$boot" '+%b %e %T')
tail -3000 /var/log/system.log | awk -v stamp="$stamp" '
    substr($0,1,15) >= stamp && /XenosBeamBlit|WindowServer crashed/ { print }
' > "$scratch/startup"
cat "$scratch/startup"
startup=1
if grep -Fq "XenosBeamBlit start pid=$windowserver_pid framebuffer_index=0" "$scratch/startup" &&
   grep -Fq "XenosBeamBlit beam pid=$windowserver_pid status=0xe00002c7" "$scratch/startup" &&
   ! grep -Fq "WindowServer crashed" "$scratch/startup"; then
    startup=0
fi
printf 'WindowServer startup fade status=%s pid=%s\n' "$startup" "$windowserver_pid"
status=0
sh RunXenosFramebufferBlit.sh --repaint > "$scratch/repaint" 2>&1 || status=$?
cat "$scratch/repaint"
test ! -e repaint-blit.arm
test ! -e repaint-run.lock
printf 'WindowServer repaint status=%s\n' "$status"
/usr/sbin/ioreg -w0 -c XenosController |
    grep -E 'XenosGPUCompletion|XenosUserQuadFence|XenosWindowServerFramebufferTrace' || :
test "$(windowserver)" = "$windowserver_pid"
if ! grep -Fq "QuartzExtreme_before=" "$scratch/repaint"; then
    sh RunXenosFramebufferBlit.sh --baseline || :
fi
test "$startup" -eq 0
exit "$status"
