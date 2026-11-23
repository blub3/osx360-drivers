set -eu
export PATH=/usr/bin:/bin:/usr/sbin:/sbin LC_ALL=C
case "$1" in /tmp/xenos-vram-source.*) ;; *) exit 2 ;; esac
source=$1
boot=/Volumes/OSX360BOOT
test "$(md5 -q "$boot/kboot.conf")" = a9c544c6bf6ab532695cc776f7804c13
/usr/sbin/ioreg -w0 -c XenonFramebufferTest | grep -Fq XenosWinFix.plugin
ps -axww -o pid,user,command |
    awk "\$3 ~ /\\/WindowServer\$/ && \$4 == \"-daemon\" { print \$1, \$2 }"
tmp=$(mktemp -d /tmp/xenos-vram-trace.XXXXXXXX)
trap "rm -rf \"$tmp\"" 0
trap "exit 1" HUP INT TERM
chmod 755 "$tmp"
install -m 755 "$source" "$tmp/probe"
test "$(md5 -q "$tmp/probe")" = 1810b654ff2e8b996fc3c7b934f7cfb1
for account in bblankenhagen windowserver; do
    echo "Read-only mapping probe for $account (no framebuffer access)"
    /usr/bin/sudo -u "$account" "$tmp/probe"
done
echo "WindowServer framebuffer mapping trace complete."
