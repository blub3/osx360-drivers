#!/bin/sh
set -eu
set -o pipefail
cd "$(dirname "$0")"
test_dir=XenosCGLWindowTest
app=XenosCGLWindow
plugin=GLEngineXenosWindow
runner=RunXenosCGLWindow.sh
file "build/$app" | grep -F 'Mach-O executable ppc'
file "build/$plugin" | grep -F 'Mach-O bundle ppc'
sh -n "$runner"
expected_app=$(md5 -q "build/$app")
expected_plugin=$(md5 -q "build/$plugin")
expected_runner=$(md5 -q "$runner")
scratch=$(mktemp -d /tmp/xenos-window-stage.XXXXXXXX)
trap 'rm -rf "$scratch"' 0
mkdir "$scratch/$test_dir"
cp "build/$app" "build/$plugin" "$runner" "$scratch/$test_dir"
remote=$(cat stage-remote.sh)
COPYFILE_DISABLE=1 tar --no-xattrs --no-mac-metadata -C "$scratch" -cf - "$test_dir" |
    ../gpu-cgl-discovery/tiger-ssh "expected_app=$expected_app
expected_plugin=$expected_plugin
expected_runner=$expected_runner
$remote"
