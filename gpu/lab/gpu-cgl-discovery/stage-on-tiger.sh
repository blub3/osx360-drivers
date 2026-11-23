#!/bin/sh
set -eu
set -o pipefail
cd "$(dirname "$0")"
archive_root=XenosCGLDiscoveryTest
runner=RunXenosCGLDiscovery.sh
app=XenosCGLDiscovery
bundle=GLDriverXenos
expected_app=$(md5 -q "build/$app")
expected_bundle=$(md5 -q "build/$bundle")
expected_runner=$(md5 -q "$runner")
temp=$(mktemp -d /tmp/xenos-cgl-stage.XXXXXXXX)
trap 'rm -rf "$temp"' 0
mkdir "$temp/$archive_root"
cp "build/$app" "build/$bundle" "$runner" "$temp/$archive_root"
remote=$(cat stage-discovery.sh)
COPYFILE_DISABLE=1 tar --no-xattrs --no-mac-metadata -C "$temp" -cf - "$archive_root" |
    ./tiger-ssh "expected_app=$expected_app
expected_bundle=$expected_bundle
expected_runner=$expected_runner
$remote"
