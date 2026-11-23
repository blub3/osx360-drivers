set -eu
umask 077
scratch=$(mktemp -d /tmp/xenos-framebuffer-link.XXXXXX)
trap 'rm -f "$scratch"/*.o "$scratch/XenonFramebufferTest"; rmdir "$scratch"' EXIT
cd "$scratch"
tar -xf -
developer=$(/usr/bin/xcode-select -print-path 2>/dev/null || printf '%s' /Developer)
for linker in /usr/bin/ld_classic "$developer/usr/bin/ld_classic" /Developer/usr/bin/ld_classic; do
    if [ -x "$linker" ]; then
        printf 'Linking with %s on %s\n' "$linker" "$(uname -p)" >&2
        "$linker" -arch ppc -r -static -o XenonFramebufferTest XenonFramebufferTest-info.o XenonFramebufferTest.o
        cat XenonFramebufferTest
        exit 0
    fi
done
echo "Xcode ld_classic was not found on the G5." >&2
exit 1
