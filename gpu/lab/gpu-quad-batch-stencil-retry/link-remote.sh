set -eu
umask 077
scratch=$(mktemp -d /tmp/xenos-quad-batch-stencil-retry-link.XXXXXX)
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
"$linker" -arch ppc -r -static -o XenonCoreGraphics-linked XenonCoreGraphics-info.o src/*.o

set -- -arch ppc -mmacosx-version-min=10.4 -std=c99 -Wall -Wextra
for sdk in "$developer/SDKs/MacOSX10.4u.sdk" /Developer/SDKs/MacOSX10.4u.sdk; do
    if [ -d "$sdk" ]; then
        set -- "$@" -isysroot "$sdk"
        break
    fi
done
/usr/bin/gcc "$@" app/XenosQuadBatchStencil.c -framework IOKit -o XenosQuadBatchStencil >&2
tar -cf - XenonCoreGraphics-linked XenosQuadBatchStencil
