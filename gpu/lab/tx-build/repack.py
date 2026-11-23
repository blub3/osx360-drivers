from pathlib import Path
import plistlib
import struct
import zlib

root = Path(__file__).parent
bundle = root / 'source/XenonEthernet/build_kext_tiger/XenonEthernet.kext/Contents'
original = (root / 'original.mkext').read_bytes()
new_plist = (bundle / 'Info.plist').read_bytes()
new_binary = (bundle / 'MacOS/XenonEthernet').read_bytes()

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
                previous = data.rfind(data[pos:pos + run], max(0, pos - 4096), pos)
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
            assert start >= 32 + 32 * count and len(raw) == (compressed or full)
            payloads.append(decode(raw, full) if compressed else raw)
        info = plistlib.loads(payloads[0])
        identifier = info['CFBundleIdentifier']
        assert identifier not in entries
        entries[identifier] = offset, *payloads
    return entries

def append_payload(archive, row, payload):
    while len(archive) % 32:
        archive.append(0)
    compressed = encode(payload)
    struct.pack_into('>4I', archive, row, len(archive), len(compressed), len(payload), 0)
    archive.extend(compressed)

entries = read_archive(original)
identifier = 'fish.goldfish64.osx360.XenonEthernet'
table_offset, old_plist, old_binary = entries[identifier]
metadata = plistlib.loads(new_plist)
assert metadata['CFBundleVersion'] == '0.0.6d1'
old_metadata = plistlib.loads(old_plist)
allowed_metadata_changes = {'CFBundleVersion', 'OSBundleCompatibleVersion'}
assert {k:v for k,v in metadata.items() if k not in allowed_metadata_changes} == {
    k:v for k,v in old_metadata.items() if k not in allowed_metadata_changes}
for name, binary in (('original', old_binary), ('updated', new_binary)):
    assert struct.unpack_from('>IIII', binary) == (0xfeedface, 18, 0, 1)
    (root / ('XenonEthernet-' + name)).write_bytes(binary)
assert b'0.0.6d1\0' in new_binary

updated = bytearray(original)
append_payload(updated, table_offset, new_plist)
append_payload(updated, table_offset + 16, new_binary)
while len(updated) % 32:
    updated.append(0)
struct.pack_into('>II', updated, 8, len(updated), zlib.adler32(updated[16:]))
reopened = read_archive(updated)
assert reopened.keys() == entries.keys()
for key, (_, plist, binary) in reopened.items():
    expected = (new_plist, new_binary) if key == identifier else entries[key][1:]
    assert (plist, binary) == expected
    print(key, plistlib.loads(plist)['CFBundleVersion'],
          'replaced' if key == identifier else 'byte-identical')
(root / 'Xbox360.mkext').write_bytes(updated)
