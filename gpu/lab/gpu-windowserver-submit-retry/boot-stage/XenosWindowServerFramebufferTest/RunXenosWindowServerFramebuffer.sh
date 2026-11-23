#!/bin/sh
set -eu
export LC_ALL=C
unset GL_RESOURCES XENOS_CGL_DISCOVERY XENOS_CGL_LEGACY_FENCE_ONLY
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
mode=${1:---require-native}
case "$mode" in
    --require-native|--baseline|--control|--association|--startup) ;;
    *) echo "Usage: $0 [--require-native|--baseline|--control|--association|--startup]" >&2; exit 2 ;;
esac
windowserver_pid=$(ps -axww -o pid,command |
    awk '$2 ~ /\/WindowServer$/ && $3 == "-daemon" { print $1 }')
case "$windowserver_pid" in
    ""|*[!0-9]*) echo "Could not identify a single Tiger WindowServer process." >&2; exit 1 ;;
esac
exec "$here/XenosWindowServerFramebufferCheck" "$mode" "$windowserver_pid"
