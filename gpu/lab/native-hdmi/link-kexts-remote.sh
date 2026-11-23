set -eu
umask 077
scratch=$(mktemp -d /tmp/xenos-native-hdmi.XXXXXX)
trap 'rm -rf "$scratch"' EXIT
cd "$scratch"
tar -xf -
developer=$(/usr/bin/xcode-select -print-path 2>/dev/null || printf '%s' /Developer)
for linker in /usr/bin/ld_classic "$developer/usr/bin/ld_classic" /Developer/usr/bin/ld_classic; do
    if [ -x "$linker" ]; then
        for driver in XenonFramebufferTest XenonSMC; do
            "$linker" -arch ppc -r -static -o "$driver" "$driver-objects/$driver-info.o" "$driver-objects"/src/*.o
        done
        tar -cf - XenonFramebufferTest XenonSMC
        exit 0
    fi
done
echo 'Xcode ld_classic was not found on the G5.' >&2
exit 1
