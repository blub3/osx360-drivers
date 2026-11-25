#!/usr/bin/env python3
"""Replace only XenonPlatform in an mkext and prove every other entry is unchanged.

Usage: repack_platform.py SOURCE.mkext XenonPlatform.kext OUTPUT.mkext [stage1|check]
"""
import plistlib
import struct
import sys
import zlib
from pathlib import Path

IDENTIFIER = 'fish.goldfish64.osx360.XenonPlatform'
ANCHOR = b'<key>IOClass</key>\n\t\t\t<string>XenonPE</string>'
TRUE = b'</key>\n\t\t\t<true/>\n\t\t\t'
FLAGS = [b'XenonPmapStats', b'XenonPmapFastScan', b'XenonPmapFlushPatch']
MODE_FLAGS = {'stage1': b'XenonPmapFlushNoEntry', 'check': b'XenonPmapFlushCheckOnly'}


def decode(data, length):
    history = bytearray(b' ' * 4096)
    cursor = 4096 - 18
    pos = 0
    result = bytearray()
    while len(result) < length:
        flags = data[pos]
        pos += 1
        for bit in range(8):
            if len(result) == length:
                break
            first = data[pos]
            pos += 1
            if flags & (1 << bit):
                values = (first,)
            else:
                second = data[pos]
                pos += 1
                offset = first | ((second & 0xF0) << 4)
                # Lazy on purpose: LZSS matches may overlap bytes written by this same run.
                values = (history[(offset + i) & 4095] for i in range((second & 15) + 3))
            for value in values:
                result.append(value)
                history[cursor] = value
                cursor = (cursor + 1) & 4095
    assert pos == len(data) and len(result) == length
    return bytes(result)


def encode(data):
    out = bytearray()
    pos = 0
    while pos < len(data):
        flag = len(out)
        out.append(0)
        for bit in range(8):
            if pos == len(data):
                break
            for run in range(min(18, len(data) - pos), 2, -1):
                previous = data.rfind(data[pos:pos + run], max(0, pos - 4096 + 18), pos)
                if previous >= 0:
                    cursor = (previous + 4096 - 18) & 4095
                    out.extend((cursor & 255, ((cursor >> 4) & 0xF0) | (run - 3)))
                    pos += run
                    break
            else:
                out[flag] |= 1 << bit
                out.append(data[pos])
                pos += 1
    return bytes(out)


def read_archive(data):
    magic, size, checksum, version, count = struct.unpack_from('>8s4I', data)
    assert magic == b'MKXTMOSX' and version == 0x01008000 and size == len(data)
    assert checksum == zlib.adler32(data[16:])
    entries = {}
    for index in range(count):
        offset = 32 + index * 32
        payloads = []
        for entry in (offset, offset + 16):
            start, compressed, full, _ = struct.unpack_from('>4I', data, entry)
            raw = data[start:start + (compressed or full)]
            assert len(raw) == (compressed or full)
            payloads.append(decode(raw, full) if compressed else raw)
        identifier = plistlib.loads(payloads[0])['CFBundleIdentifier']
        assert identifier not in entries
        entries[identifier] = (offset, payloads[0], payloads[1])
    return entries


def append_payload(archive, row, payload):
    while len(archive) % 32:
        archive.append(0)
    compressed = encode(payload)
    assert decode(compressed, len(payload)) == payload
    struct.pack_into('>4I', archive, row, len(archive), len(compressed), len(payload), 0)
    archive.extend(compressed)


def main():
    source, kext, output = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
    mode = sys.argv[4] if len(sys.argv) > 4 else None
    if mode == 'noflush':
        keys = [k for k in FLAGS if k != b'XenonPmapFlushPatch']
    elif mode == 'nofast':
        keys = [k for k in FLAGS if k != b'XenonPmapFastScan']
    elif mode == 'asis':
        keys = []
    else:
        keys = FLAGS + ([MODE_FLAGS[mode]] if mode else [])

    original = source.read_bytes()
    new_plist = (kext / 'Contents/Info.plist').read_bytes()
    new_binary = (kext / 'Contents/MacOS/XenonPlatform').read_bytes()
    if mode == 'asis':
        keys = []
    else:
        assert new_plist.count(ANCHOR) == 1 and b'XenonPmap' not in new_plist
        new_plist = new_plist.replace(ANCHOR, b''.join(b'<key>' + k + TRUE for k in keys) + ANCHOR)
    personality = plistlib.loads(new_plist)['IOKitPersonalities']['XenonPE']
    assert all(personality[k.decode()] is True for k in keys)

    entries = read_archive(original)
    row = entries[IDENTIFIER][0]
    updated = bytearray(original)
    append_payload(updated, row, new_plist)
    append_payload(updated, row + 16, new_binary)
    while len(updated) % 32:
        updated.append(0)
    struct.pack_into('>II', updated, 8, len(updated), zlib.adler32(bytes(updated[16:])))

    reopened = read_archive(bytes(updated))
    assert reopened.keys() == entries.keys()
    for key, (_, plist, binary) in reopened.items():
        expected = (new_plist, new_binary) if key == IDENTIFIER else entries[key][1:]
        assert (plist, binary) == expected
        print(key.rsplit('.', 1)[1], 'replaced' if key == IDENTIFIER else 'byte-identical')
    with output.open('xb') as handle:
        handle.write(updated)


main()
