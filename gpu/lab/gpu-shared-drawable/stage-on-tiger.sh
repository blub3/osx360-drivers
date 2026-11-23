#!/bin/sh
set -eu
set -o pipefail
export LC_ALL=C
root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
python3 "$root/package-shared-drawable.py"
stage="$root/boot-stage"
archive=XenosSHR.mkext
elf=openbios-xbox360-gpu-shared-drawable.elf
config=kboot-gpu-shared-drawable.conf
app=XenosSharedDrawable
archive_md5=$(/sbin/md5 -q "$stage/$archive")
elf_md5=$(/sbin/md5 -q "$stage/$elf")
config_md5=$(/sbin/md5 -q "$stage/$config")
app_md5=$(/sbin/md5 -q "$stage/$app")
remote_script=$(cat "$root/stage-shared-drawable.sh")
COPYFILE_DISABLE=1 tar --no-xattrs --no-mac-metadata -C "$stage" -cf - "$archive" "$elf" "$config" "$app" |
ssh -T -o BatchMode=yes -o IdentitiesOnly=yes -i "$HOME/.ssh/id_rsa_osx360" -o ConnectTimeout=5 \
    -o ServerAliveInterval=5 -o ServerAliveCountMax=2 -o HostKeyAlgorithms=ssh-rsa \
    -o PubkeyAcceptedAlgorithms=ssh-rsa -o StrictHostKeyChecking=yes \
    -o UserKnownHostsFile="$HOME/.ssh/known_hosts_osx360" ${OSX360_TIGER_HOST} "archive_md5=$archive_md5
elf_md5=$elf_md5
config_md5=$config_md5
app_md5=$app_md5
$remote_script"
