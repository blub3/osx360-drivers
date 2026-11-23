#!/bin/sh
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
xbox() {
    ssh -T -o BatchMode=yes -o IdentitiesOnly=yes -i "$HOME/.ssh/id_rsa_osx360" \
        -o ConnectTimeout=5 -o ServerAliveInterval=5 -o ServerAliveCountMax=2 \
        -o HostKeyAlgorithms=ssh-rsa -o PubkeyAcceptedAlgorithms=ssh-rsa \
        -o StrictHostKeyChecking=yes -o UserKnownHostsFile="$HOME/.ssh/known_hosts_osx360" \
        ${OSX360_TIGER_HOST} "$@"
}
previous_boot=$1
case "$previous_boot" in ''|*[!0-9]*) exit 2 ;; esac
deadline=$(($(date +%s) + 720))
latest=
while test "$(date +%s)" -lt "$deadline"; do
    if probe=$(printf '%s\n' "$previous_boot" | xbox '
        read -r previous_boot || exit 1
        now=$(/usr/sbin/sysctl -n kern.boottime) || exit 1
        test "$now" -gt "$previous_boot" || exit 2
        printf "New Tiger boot %s\n" "$now"
        test -x /Volumes/OSX360BOOT/XenosQuadBatchDepth || exit 3
        /usr/sbin/kextstat | grep -F fish.goldfish64.osx360.XenonCoreGraphics
    ' 2>&1); then
        printf '%s\n' "$probe"
        if printf '%s\n' "$probe" | grep -F 'XenonCoreGraphics (0.0.5d27)' >/dev/null; then
            xbox "/bin/sh -s -- $previous_boot" < "$here/verify-quad-batch-depth.sh"
            exit $?
        fi
        echo 'Tiger loaded a different XenonCoreGraphics version.'
        exit 4
    fi
    if test "$latest" != "$probe" && test -n "$probe"; then
        printf '%s\n' "$probe"
    fi
    latest=$probe
    sleep 12
done
echo 'Timed out waiting for a new Tiger boot with the USB test available.'
exit 5
