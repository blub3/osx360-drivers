#!/bin/bash
set -euo pipefail
export LC_ALL=C
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
stage="$here/boot-stage"

xbox() {
    ssh -T -o BatchMode=yes -o IdentitiesOnly=yes -i "$HOME/.ssh/id_rsa_osx360" \
        -o ConnectTimeout=5 -o ServerAliveInterval=5 -o ServerAliveCountMax=2 \
        -o HostKeyAlgorithms=ssh-rsa -o PubkeyAcceptedAlgorithms=ssh-rsa \
        -o StrictHostKeyChecking=yes -o UserKnownHostsFile="$HOME/.ssh/known_hosts_osx360" \
        ${OSX360_TIGER_HOST} "$@"
}

deadline=$(($(date +%s) + 600))
last=
ready=0
while test "$(date +%s)" -lt "$deadline"; do
    if probe=$(xbox '
        boot=$(/usr/sbin/sysctl -n kern.boottime) || exit 1
        printf "Tiger boot %s\n" "$boot"
        test -f /Volumes/OSX360BOOT/kboot.conf || exit 3
        /usr/sbin/kextstat | grep XenonCoreGraphics
    ' </dev/null 2>&1); then
        printf '%s\n' "$probe"
        ready=1
        break
    fi
    if test "$last" != "$probe" && test -n "$probe"; then
        printf '%s\n' "$probe"
    fi
    last=$probe
    sleep 12
done
if test "$ready" -ne 1; then
    echo 'Timed out waiting for Tiger and OSX360BOOT to return.'
    exit 5
fi

archive=XenosQSR.mkext
elf=openbios-xbox360-gpu-quad-batch-stencil-retry.elf
config=kboot-gpu-quad-batch-stencil-retry.conf
app=XenosQuadBatchStencilRetry
archive_md5=$(/sbin/md5 -q "$stage/$archive")
elf_md5=$(/sbin/md5 -q "$stage/$elf")
config_md5=$(/sbin/md5 -q "$stage/$config")
app_md5=$(/sbin/md5 -q "$stage/$app")
remote_script=$(cat "$here/stage-quad-batch-stencil-retry.sh")
COPYFILE_DISABLE=1 tar --no-xattrs --no-mac-metadata -C "$stage" -cf - "$archive" "$elf" "$config" "$app" |
xbox "archive_md5=$archive_md5
elf_md5=$elf_md5
config_md5=$config_md5
app_md5=$app_md5
$remote_script"

xbox /bin/sh < "$here/activate-quad-batch-stencil-retry.sh"
