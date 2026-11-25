#!/usr/bin/env python3
"""Replace one kext (plist + binary, as is) in an mkext and prove every other entry is unchanged.

Usage: repack_kext.py SOURCE.mkext BUNDLE_ID KEXT_DIR OUTPUT.mkext
"""
import sys
from pathlib import Path

src = (Path(__file__).parent / "repack_platform.py").read_text().replace("\nmain()\n", "\n")
ns = {"__name__": "repack_platform"}
exec(compile(src, "repack_platform.py", "exec"), ns)
read_archive, append_payload = ns["read_archive"], ns["append_payload"]
import struct, zlib

source, ident, kext, output = Path(sys.argv[1]), sys.argv[2], Path(sys.argv[3]), Path(sys.argv[4])
name = ident.rsplit(".", 1)[1]
new_plist = (kext / "Contents/Info.plist").read_bytes()
new_binary = (kext / "Contents/MacOS" / name).read_bytes()
original = source.read_bytes()
entries = read_archive(original)
row = entries[ident][0]
updated = bytearray(original)
append_payload(updated, row, new_plist)
append_payload(updated, row + 16, new_binary)
while len(updated) % 32:
    updated.append(0)
struct.pack_into(">II", updated, 8, len(updated), zlib.adler32(bytes(updated[16:])))
reopened = read_archive(bytes(updated))
assert reopened.keys() == entries.keys()
for key, (_, plist, binary) in reopened.items():
    expected = (new_plist, new_binary) if key == ident else entries[key][1:]
    assert (plist, binary) == expected
    print(key.rsplit(".", 1)[1], "replaced" if key == ident else "byte-identical")
with output.open("xb") as handle:
    handle.write(updated)
