#!/bin/sh
set -eu
cd "$(dirname "$0")"
for item do
    destination=reference/tiger-root$item
    mkdir -p "$(dirname "$destination")"
    tmp=$destination.part
    ./tiger-ssh "cat '$item'" > "$tmp"
    mv "$tmp" "$destination"
done
