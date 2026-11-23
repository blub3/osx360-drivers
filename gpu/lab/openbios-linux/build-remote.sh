set -eu
umask 077
root=$(mktemp -d /tmp/openbios-hdmi-build.XXXXXX)
printf 'Linux build directory: %s\n' "$root" >&2
git clone --single-branch --branch codex/hdmi-audio-test https://github.com/blub3/openbios.git "$root/source" >&2
cd "$root/source"
revision=$(git rev-parse HEAD)
test "$revision" = 9553e75981fae600de047e0f2a3404d4e9db115c
printf 'Building commit %s on %s\n' "$revision" "$(uname -sm)" >&2
if ./config/scripts/switch-arch xbox360-ppc >&2 && make -j2 >&2; then
    :
else
    printf 'Native build unavailable, trying the project build container.\n' >&2
    if command -v docker >/dev/null && docker info >/dev/null 2>&1; then
        engine=docker
    elif command -v podman >/dev/null && podman info >/dev/null 2>&1; then
        engine=podman
    else
        echo 'No usable Docker or Podman runtime under this SSH account.' >&2
        exit 1
    fi
    git clone --local "$root/source" "$root/container-source" >&2
    "$engine" run --rm --user "$(id -u):$(id -g)" -v "$root/container-source:/src" -w /src \
        ghcr.io/openbios/openbios-builder:master sh -c './config/scripts/switch-arch xbox360-ppc && make -j2' >&2
    cd "$root/container-source"
fi
test -s obj-ppc/openbios-xbox360.elf
sha256sum obj-ppc/openbios-xbox360.elf >&2
cat obj-ppc/openbios-xbox360.elf
