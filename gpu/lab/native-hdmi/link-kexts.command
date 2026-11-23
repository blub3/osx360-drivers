#!/bin/sh
set -eu
umask 077
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
stage=$(mktemp "$here/.linked-native-hdmi.XXXXXX")
trap 'rm -f "$stage"' EXIT
ssh -T -o ConnectTimeout=8 -o ServerAliveInterval=8 -o ServerAliveCountMax=2 \
    -o StrictHostKeyChecking=yes -o UserKnownHostsFile="$here/../tx-build/known_hosts_g5" \
    -o HostKeyAlgorithms=+ssh-rsa ${OSX360_G5_HOST} \
    "$(cat "$here/link-kexts-remote.sh")" < "$here/link-input.tar" > "$stage"
tar -tf "$stage"
mv "$stage" "$here/linked-native-hdmi.tar"
echo 'Native HDMI kext link complete.'
