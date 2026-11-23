set -eu
export LC_ALL=C
boot=/Volumes/OSX360BOOT
/sbin/mount | grep -Fq " on $boot ("
cd "$boot"
check() {
    if test "$(md5 -q "$2")" != "$1"; then
        echo "Checksum mismatch for $2" >&2
        exit 1
    fi
}
check 307278fc63c1ba863a75d313af0ba14b kboot.conf
check c76086865a738b203ebe3aab6f628dd5 kboot.conf.before-gpufbblit.bak
check d4e5729d0e58cd7b12194d3469f59509 kboot.conf.before-gpucompbootfix.bak
check 3d5be96c3df8c4c3cdfc514c7c637ba1 CMPFIX.ELF
check c603562c9452658c09f5e25db66ff936 XWCF.MKE
check b99d4be9377e3e7965bb3e88d5f1ff0e NativeHD.mkext
check ca72c036347285e3fe77b58d3b2c93c6 openbios-xbox360-native-hdmi-test.elf
check 2ce8cf67dd57d0ef8b68beaa342e051a openbios-xbox360.720p-hdmi.elf.bak
check a1f4dee1cea39d416ec981989d254556 XWBL.MKE
check 06b186cf2937bd8a03af7f43c525ed6e BLITSURF.ELF
printf 'boot=%s\n' "$(/usr/sbin/sysctl -n kern.boottime)"
/usr/sbin/kextstat | grep -E 'fish.goldfish64.osx360.Xenon(FramebufferTest|CoreGraphics)'
/usr/sbin/ioreg -w0 -c XenonFramebufferTest | grep -E 'IOCFPlugInTypes|IOAccelRevision|IOGLBundleName|XenosWindowServerFramebufferPID' || :
cd XenosFramebufferBlitTest
check 92548b692ff8ec82ab37c58357fc928d RunXenosFramebufferBlit.sh
check fee59fcdea248898dc4216419c11ce85 XenosFramebufferBlitCheck
check 1a8a669b9bbd212c9183af342aaab566 XenosFramebufferRepaintCheck
status=0
sh RunXenosFramebufferBlit.sh --repaint || status=$?
test ! -e repaint-blit.arm
test ! -e repaint-run.lock
printf 'WindowServer repaint status=%s\n' "$status"
/usr/sbin/ioreg -w0 -c XenosController | grep -E 'XenosGPUCompletion|XenosUserQuadFence|XenosWindowServerFramebufferTrace' || :
echo 'Recent WindowServer blit plug-in messages'
tail -1500 /var/log/system.log | grep -F XenosLegacyBlit | tail -20 || :
if [ "$status" -ne 0 ]; then
    sh RunXenosFramebufferBlit.sh --baseline || :
fi
exit "$status"
