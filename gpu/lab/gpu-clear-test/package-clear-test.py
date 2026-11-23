import ast
import hashlib
from pathlib import Path
import plistlib
import struct
import zlib


root = Path(__file__).resolve().parent
codec_path = root.parent / "tx-build/repack.py"
codec_ast = ast.parse(codec_path.read_text())
codec_ast.body = [node for node in codec_ast.body if isinstance(node, ast.FunctionDef)]
codec = {"struct": struct, "zlib": zlib, "plistlib": plistlib}
exec(compile(codec_ast, str(codec_path), "exec"), codec)

base = root.parent / "gpu-marker-test/boot-stage"
original = (base / "XenosCMD.mkext").read_bytes()
assert hashlib.md5(original).hexdigest() == "f56201ddaec0cdf991048fef49a65835"
entries = codec["read_archive"](original)
archive = bytearray(original[:32 + 32 * len(entries)])
target = "fish.goldfish64.osx360.XenonCoreGraphics"
plist = (root / "source/XenonCoreGraphics/build_tiger/XenonCoreGraphics-Info.plist").read_bytes()
binary = (root / "XenonCoreGraphics-linked").read_bytes()
assert struct.unpack_from(">4I", binary) == (0xfeedface, 18, 0, 1)
assert b"0.0.5d3\0" in binary and b"XenosRing: rptr=" in binary
assert b"XenosMarker: %s MEM_WRITE" in binary
assert b"XenosClear: PASS 32x32 pitch=" in binary and b"XenosClearPixels" in binary
old_info = plistlib.loads(entries[target][1])
new_info = plistlib.loads(plist)
for key in ("CFBundleVersion", "OSBundleCompatibleVersion"):
    assert old_info.pop(key) == "0.0.5d2"
    assert new_info.pop(key) == "0.0.5d3"
assert old_info == new_info

for identifier, (row, _, _) in entries.items():
    if identifier == target:
        codec["append_payload"](archive, row, plist)
        codec["append_payload"](archive, row + 16, binary)
        continue
    for slot in (row, row + 16):
        offset, compressed, length, _ = struct.unpack_from(">4I", original, slot)
        archive.extend(bytes(-len(archive) % 32))
        struct.pack_into(">I", archive, slot, len(archive))
        archive.extend(original[offset:offset + (compressed or length)])

archive.extend(bytes(-len(archive) % 32))
struct.pack_into(">II", archive, 8, len(archive), zlib.adler32(archive[16:]))
new_entries = codec["read_archive"](archive)
assert new_entries.keys() == entries.keys()
for identifier, (_, unpacked_plist, unpacked_binary) in new_entries.items():
    expected = (plist, binary) if identifier == target else entries[identifier][1:]
    assert (unpacked_plist, unpacked_binary) == expected, identifier

elf = (base / "openbios-xbox360-gpu-marker-test.elf").read_bytes()
assert hashlib.md5(elf).hexdigest() == "f78441e4a22cdef099bf1d7ed3e69bca"
name = b"XenosCMD.mkext\0"
clear_name = b"XenosCLR.mkext\0"
assert len(name) == len(clear_name) and elf.count(name) == 1 and clear_name not in elf
test_elf = elf.replace(name, clear_name)
assert test_elf.replace(clear_name, name) == elf
elf_name = "openbios-xbox360-gpu-clear-test.elf"

config = (base / "kboot-gpu-marker-test.conf").read_bytes()
assert hashlib.md5(config).hexdigest() == "5b7955d3eec06d9f6e1dbaeae47d0b2b"
assert config.count(b"default=gpumarker\n") == 1 and config.endswith(b"\n")
config = config.replace(b"default=gpumarker\n", b"default=gpuclear\n")
config += f'gpuclear="uda0:/{elf_name}"\n'.encode()

stage = root / "boot-stage"
stage.mkdir(exist_ok=True)
files = {"XenosCLR.mkext": archive, elf_name: test_elf, "kboot-gpu-clear-test.conf": config}
for name, data in files.items():
    (stage / name).write_bytes(data)
    print(f"{hashlib.md5(data).hexdigest()}  {len(data):>7}  {name}")
