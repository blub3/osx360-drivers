#!/bin/sh
set -eu
umask 077
if [ "$#" -ne 1 ]; then
    printf '%s\n' "Usage: $0 username@G5-address" >&2
    exit 2
fi
root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
stage=$(mktemp "$root/.G5-kext.XXXXXX")
trap 'rm -f "$stage"' EXIT
trap 'exit 1' HUP INT TERM
ssh -T -o ConnectTimeout=10 -o ServerAliveInterval=8 -o ServerAliveCountMax=2 \
    -o HostKeyAlgorithms=+ssh-rsa -o PubkeyAcceptedAlgorithms=+ssh-rsa \
    -o UserKnownHostsFile="$root/known_hosts_g5" -o StrictHostKeyChecking=ask \
    -- "$1" "$(cat "$root/classic-link-remote.sh")" \
    < "$root/classic-link-input.tar" > "$stage"
file "$stage" | grep -q 'Mach-O object ppc'
mv -f "$stage" "$root/XenonEthernet-g5"
echo "The G5 linked XenonEthernet successfully. Ready to package."
