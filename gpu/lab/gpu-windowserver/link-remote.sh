set -eu
umask 077
scratch=$(mktemp -d /tmp/xenos-windowserver-link.XXXXXX)
trap 'rm -rf "$scratch"' EXIT
cd "$scratch"
tar -xf -
developer=$(/usr/bin/xcode-select -print-path 2>/dev/null || printf '%s' /Developer)
linker=
for candidate in /usr/bin/ld_classic "$developer/usr/bin/ld_classic" /Developer/usr/bin/ld_classic; do
    if [ -x "$candidate" ]; then
        linker=$candidate
        break
    fi
done
test -n "$linker"
"$linker" -arch ppc -r -static -o XenonCoreGraphics-linked build_tiger/XenonCoreGraphics-info.o src/*.o

tar -cf - XenonCoreGraphics-linked
