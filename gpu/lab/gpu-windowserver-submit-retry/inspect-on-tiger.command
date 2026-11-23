#!/bin/sh
set -eu
set -o pipefail
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
pid=$("$here/../gpu-cgl-discovery/tiger-ssh" 'ps -axww -o pid,command | awk '\''$2 ~ /\/WindowServer$/ && $3 == "-daemon" { print $1 }'\''')
case "$pid" in ''|*[!0-9]*) echo "Could not identify the single WindowServer process." >&2; exit 1 ;; esac
ssh -tt -o BatchMode=yes -o IdentitiesOnly=yes -i "$HOME/.ssh/id_rsa_osx360" \
    -o ConnectTimeout=5 -o ServerAliveInterval=5 -o ServerAliveCountMax=2 \
    -o HostKeyAlgorithms=ssh-rsa -o PubkeyAcceptedAlgorithms=ssh-rsa \
    -o StrictHostKeyChecking=yes -o UserKnownHostsFile="$HOME/.ssh/known_hosts_osx360" \
    ${OSX360_TIGER_HOST} "/usr/bin/sudo /bin/sh -c '
set -eu
status=0
map=\$(/usr/bin/vmmap $pid 2>&1) || status=\$?
echo \"WindowServer PID $pid vmmap status \$status\"
if test \"\$status\" -ne 0; then
    printf \"%s\\n\" \"\$map\"
    exit \"\$status\"
fi
printf \"%s\\n\" \"\$map\" | /usr/bin/grep -E \"(OpenGL.framework|GLDriverXenosWSProbe|/Resources/WindowServer)\" || :
echo \"WindowServer library check complete.\"
'" | tee "$here/windowserver-vmmap.log"
