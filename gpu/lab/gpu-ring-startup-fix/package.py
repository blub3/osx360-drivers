import ast
import hashlib
from pathlib import Path
import plistlib
import struct
import zlib

root = Path(__file__).resolve().parent
base_dir = root.parent / "gpu-window-batched-overlap" / "boot-stage"
stage = root / "boot-stage"
stage.mkdir(exist_ok=True)

codec_path = root.parent / "tx-build" / "repack.py"
codec_module = ast.parse(codec_path.read_text())
codec_module.body = [node for node in codec_module.body if isinstance(node, ast.FunctionDef)]
codec = {"struct": struct, "zlib": zlib, "plistlib": plistlib}
exec(compile(codec_module, str(codec_path), "exec"), codec)


def verified(path, digest):
    data = path.read_bytes()
    if hashlib.md5(data).hexdigest() != digest:
        raise ValueError(f"Checksum mismatch for {path}")
    return data


base = verified(base_dir / "XWBT.MKE", "abb54eca183cb316bd4e8cdad7dd8ac1")
original = codec["read_archive"](base)
identifier = "fish.goldfish64.osx360.XenonCoreGraphics"
row, old_plist, _ = original[identifier]
assert old_plist.count(b"0.0.5d45") == 2
plist = old_plist.replace(b"0.0.5d45", b"0.0.5d46")
binary = (root / "XenonCoreGraphics-linked").read_bytes()
assert struct.unpack_from(">4I", binary) == (0xfeedface, 18, 0, 1)
module = identifier.encode().ljust(64, b"\0")
assert binary.count(module + b"0.0.5d46".ljust(64, b"\0")) == 1
assert b"XenosRingStartupReady\0" in binary and b"XenosRingStartupAttempts\0" in binary

plist_offset, _, _, plist_stamp = struct.unpack_from(">4I", base, row)
binary_offset, _, _, binary_stamp = struct.unpack_from(">4I", base, row + 16)
tail_offset = struct.unpack_from(">I", base, row + 32)[0]
plist_data = codec["encode"](plist)
binary_data = codec["encode"](binary)
assert len(plist_data) <= binary_offset - plist_offset
needed = max(0, binary_offset + len(binary_data) - tail_offset)
shift = (needed + 31) & ~31
archive = bytearray(base[:tail_offset]) + bytes(shift) + base[tail_offset:]
archive[plist_offset:binary_offset] = plist_data.ljust(binary_offset - plist_offset, b"\0")
archive[binary_offset:tail_offset + shift] = binary_data.ljust(tail_offset + shift - binary_offset, b"\0")
struct.pack_into(">4I", archive, row, plist_offset, len(plist_data), len(plist), plist_stamp)
struct.pack_into(">4I", archive, row + 16, binary_offset, len(binary_data), len(binary), binary_stamp)
for record, _, _ in original.values():
    if record > row:
        for payload_row in (record, record + 16):
            old_offset = struct.unpack_from(">I", base, payload_row)[0]
            assert old_offset >= tail_offset
            struct.pack_into(">I", archive, payload_row, old_offset + shift)
struct.pack_into(">II", archive, 8, len(archive), zlib.adler32(archive[16:]))

decoded = codec["read_archive"](archive)
assert decoded.keys() == original.keys()
for name, (record, metadata, executable) in decoded.items():
    assert record == original[name][0]
    assert (metadata, executable) == ((plist, binary) if name == identifier else original[name][1:])
    if name != identifier:
        for field in (0, 16):
            before = struct.unpack_from(">4I", base, record + field)
            after = struct.unpack_from(">4I", archive, record + field)
            assert after == (before[0] + (shift if record > row else 0), *before[1:])
print(f"Kept eight unchanged kexts. Archive {len(base)} -> {len(archive)} bytes, tail shift={shift}.")

firmware = verified(base_dir / "WINBATCH.ELF", "04cfc858e927a09f4d922e0d07ead5c4")
assert firmware.count(b"XWBT.MKE\0") == 1 and b"XWRF.MKE\0" not in firmware
firmware = firmware.replace(b"XWBT.MKE\0", b"XWRF.MKE\0")
config = verified(base_dir / "kboot-gpu-window-batch.conf", "c8d3080f6df7a8e63aa6c66a2a5a1db3")
lines = config.splitlines()
assert lines[:4] == [b"#KBOOTCONFIG", b"default=gpuwinbatch", b"timeout=0",
                     b'gpuwinbatch="uda0:/WINBATCH.ELF"']
entries = [b'gpuringfix="uda0:/RINGFIX.ELF"', *lines[3:]]
assert len(entries) <= 64 and len({line.split(b"=", 1)[0] for line in entries}) == len(entries)
config = b"\n".join([lines[0], b"default=gpuringfix", lines[2], *entries]) + b"\n"
print(f"Retained {len(entries) - 1} existing boot entries.")
for name, data in (("RINGFIX.ELF", firmware), ("XWRF.MKE", archive),
                   ("kboot-gpu-ring-startup.conf", config)):
    (stage / name).write_bytes(data)
