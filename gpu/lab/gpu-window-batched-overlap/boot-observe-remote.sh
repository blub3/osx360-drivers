set -eu
export LC_ALL=C
boot=$(/usr/sbin/sysctl -n kern.boottime)
case "$boot" in ''|*[!0-9]*) exit 1 ;; esac
profile=$(/usr/sbin/ioreg -w0 -c XenonFramebufferTest)
case "$profile" in *XenosWinBatch.plugin*) registration=batch ;; *) registration=other ;; esac
mounted=no
if /sbin/mount | grep -Fq ' on /Volumes/OSX360BOOT ('; then
    mounted=yes
fi
processes=$(ps -axww -o pid,command | awk '
    $2 ~ /\/WindowServer$/ && $3 == "-daemon" { server=$1 }
    $2 ~ /\/Finder$/ { finder=$1 }
    $2 ~ /\/Dock$/ { dock=$1 }
    END { print server+0, finder+0, dock+0 }')
stamp=$(date -r "$boot" '+%b %e %T')
crashes=$(tail -2000 /var/log/system.log | awk -v stamp="$stamp" '
    substr($0,1,15) >= stamp && /WindowServer crashed/ { n++ }
    END { print n+0 }')
printf '%s %s %s %s %s\n' "$boot" "$registration" "$mounted" "$processes" "$crashes"
