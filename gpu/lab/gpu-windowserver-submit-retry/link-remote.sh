set -eu
umask 077
scratch=$(mktemp -d /tmp/xenos-ws-framebuffer-link.XXXXXX)
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
for driver in XenonCoreGraphics XenonFramebufferTest; do
    "$linker" -arch ppc -r -static -o "$driver-linked" \
        "$driver/build_tiger/build_tiger/$driver-info.o" "$driver/build_tiger/src/"*.o
done
tar -cf - XenonCoreGraphics-linked XenonFramebufferTest-linked
