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
check 348ca1649e90bea8a13541525f9ac0d6 kboot.conf
check 92b5d269d906ca20b85264513d97e5b6 kboot.conf.before-gpuwinrepaint.bak
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
check c45190432a777666157c3d7a6e346f0e XWRP.MKE
check b0e25ffc8fecbd7a0e55ab9300122e24 WINPAINT.ELF
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
cd XenosWindowRepaintTest
check 2e5ef7008514c68ed7825567df47fc69 RunXenosWindowRepaint.sh
check 7176160963361f234d7e0f080a5e0d96 XenosWindowPluginCheck
check a3e573ba20dd0b1d4bbafbfac95ba3cb XenosWindowRepaintCheck
stamp=$(date -r "$boot" '+%b %e %T')
tail -3000 /var/log/system.log | awk -v stamp="$stamp" '
    substr($0,1,15) >= stamp && /XenosWinBlit (start|beam)|WindowServer crashed/ { print }'
status=0
sh RunXenosWindowRepaint.sh --repaint || status=$?
test ! -e repaint-window.arm
test ! -e repaint-window.lock
printf 'WindowServer repaint status=%s\n' "$status"
/usr/sbin/ioreg -w0 -c XenosController |
    grep -E 'XenosGPUCompletion|XenosUserQuadFence|XenosWindowServerFramebufferTrace' || :
test "$(windowserver)" = "$windowserver_pid"
exit "$status"
