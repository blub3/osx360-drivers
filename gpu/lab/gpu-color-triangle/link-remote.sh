set -eu
umask 077
scratch=$(mktemp -d /tmp/xenos-color-triangle-link.XXXXXX)
trap 'rm -rf "$scratch"' EXIT
cd "$scratch"
tar -xf -
developer=$(/usr/bin/xcode-select -print-path 2>/dev/null || printf '%s' /Developer)
for linker in /usr/bin/ld_classic "$developer/usr/bin/ld_classic" /Developer/usr/bin/ld_classic; do
    if [ -x "$linker" ]; then
        "$linker" -arch ppc -r -static -o XenonCoreGraphics XenonCoreGraphics-info.o src/*.o
        cat XenonCoreGraphics
        exit 0
    fi
done
echo 'Xcode ld_classic was not found on the G5.' >&2
exit 1
