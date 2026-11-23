set -eu
export LC_ALL=C
boot=$(/usr/sbin/sysctl -n kern.boottime)
case "$boot" in ''|*[!0-9]*) exit 1 ;; esac
/sbin/mount | grep -Fq ' on /Volumes/OSX360BOOT ('
test -f /Volumes/OSX360BOOT/XenosCompositorRetryTest/XenosTransitionProbe
/usr/sbin/kextstat | grep -Fq 'fish.goldfish64.osx360.XenonCoreGraphics (0.0.5d45)'
/usr/sbin/ioreg -w0 -c XenonFramebufferTest |
    grep -Fq '"IOGLBundleName" = "GLDriverXenosCompositorRetry"'
processes=$(ps -axww -o pid,command | awk '
    $2 ~ /\/WindowServer$/ && $3 == "-daemon" { server=$1 }
    $2 ~ /\/Finder$/ { finder=$1 }
    $2 ~ /\/Dock$/ { dock=$1 }
    END {
        if (!server || !finder || !dock) exit 1
        print server, finder, dock
    }')
printf '%s %s\n' "$boot" "$processes"
