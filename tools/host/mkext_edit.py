#!/usr/bin/env python3
"""Build an mkext from another one: drop kexts, replace a kext's plist and/or binary, keep the rest.

Usage: mkext_edit.py SOURCE.mkext OUTPUT.mkext [--drop BUNDLE_ID]... [--plist BUNDLE_ID=Info.plist]...
                     [--binary BUNDLE_ID=executable]...

Unchanged kexts keep their compressed bytes exactly. The result is decoded again and every kext is
checked: dropped ones are gone, replaced parts match the new files, all others are byte-identical.
"""
import argparse
import struct
import zlib
from pathlib import Path

src = (Path(__file__).parent / "repack_platform.py").read_text().replace("\nmain()\n", "\n")
ns = {"__name__": "repack_platform"}
exec(compile(src, "repack_platform.py", "exec"), ns)
read_archive, append_payload = ns["read_archive"], ns["append_payload"]


def pairs(values):
    result = {}
    for value in values or []:
        key, path = value.split("=", 1)
        result[key] = Path(path).read_bytes()
    return result


parser = argparse.ArgumentParser()
parser.add_argument("source", type=Path)
parser.add_argument("output", type=Path)
parser.add_argument("--drop", action="append", default=[])
parser.add_argument("--plist", action="append")
parser.add_argument("--binary", action="append")
args = parser.parse_args()
plists, binaries = pairs(args.plist), pairs(args.binary)

data = args.source.read_bytes()
entries = read_archive(data)
for key in list(args.drop) + list(plists) + list(binaries):
    assert key in entries, f"{key} is not in {args.source}"

magic, size, checksum, version, count, cputype, cpusubtype = struct.unpack_from(">8s4I2I", data)
kept = [key for key in sorted(entries, key=lambda k: entries[k][0]) if key not in args.drop]
archive = bytearray(32 + 32 * len(kept))
for index, key in enumerate(kept):
    row = 32 + 32 * index
    old_row = entries[key][0]
    for part, replacement in ((0, plists.get(key)), (1, binaries.get(key))):
        desc = row + 16 * part
        start, compressed, full, modified = struct.unpack_from(">4I", data, old_row + 16 * part)
        if replacement is not None:
            append_payload(archive, desc, replacement)
        elif full == 0:
            struct.pack_into(">4I", archive, desc, 0, 0, 0, modified)
        else:
            while len(archive) % 32:
                archive.append(0)
            struct.pack_into(">4I", archive, desc, len(archive), compressed, full, modified)
            archive.extend(data[start:start + (compressed or full)])
while len(archive) % 32:
    archive.append(0)
struct.pack_into(">8s4I2I", archive, 0, magic, len(archive), 0, version, len(kept), cputype, cpusubtype)
struct.pack_into(">I", archive, 12, zlib.adler32(bytes(archive[16:])))

reopened = read_archive(bytes(archive))
assert set(reopened) == set(kept)
for key in sorted(entries):
    if key in args.drop:
        print(key.rsplit(".", 1)[1], "dropped")
        continue
    _, plist, binary = reopened[key]
    want_plist = plists.get(key, entries[key][1])
    want_binary = binaries.get(key, entries[key][2])
    assert plist == want_plist and binary == want_binary, key
    changed = [name for name, table in (("plist", plists), ("binary", binaries)) if key in table]
    print(key.rsplit(".", 1)[1], ("replaced " + "+".join(changed)) if changed else "byte-identical")
with args.output.open("xb") as handle:
    handle.write(archive)
