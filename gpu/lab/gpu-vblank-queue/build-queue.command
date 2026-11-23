#!/bin/sh
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
"$here/link-on-g5.command"
"$here/build-openbios.command"
echo 'Queued VBlank kext and OpenBIOS build complete.'
