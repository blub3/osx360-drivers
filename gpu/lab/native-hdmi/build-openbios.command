#!/bin/sh
set -eu
umask 077
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
stage=$(mktemp "$here/.openbios-native-hdmi.XXXXXX")
trap 'rm -f "$stage"' EXIT
ssh -T -o ConnectTimeout=10 -o ServerAliveInterval=10 -o ServerAliveCountMax=3 \
    -o StrictHostKeyChecking=yes -o UserKnownHostsFile="$here/../openbios-linux/known_hosts" \
    ${OSX360_BUILD_HOST} "$(cat "$here/../openbios-linux/build-1080p-remote.sh")" \
    < "$here/openbios-source.tar" > "$stage"
file "$stage" | grep -i 'ELF.*PowerPC'
mv "$stage" "$here/openbios-xbox360-native-hdmi-test.elf"
echo 'Native HDMI OpenBIOS build complete.'
