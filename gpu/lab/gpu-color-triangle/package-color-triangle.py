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

base = root.parent / "gpu-triangle-retry/boot-stage"
original = (base / "XenosTR2.mkext").read_bytes()
assert hashlib.md5(original).hexdigest() == "d17247c03ac273a63b387dce42017813"
entries = codec["read_archive"](original)
archive = bytearray(original[:32 + 32 * len(entries)])
target = "fish.goldfish64.osx360.XenonCoreGraphics"
plist = (root / "source/XenonCoreGraphics/build_tiger/XenonCoreGraphics-Info.plist").read_bytes()
binary = (root / "XenonCoreGraphics-linked").read_bytes()
assert struct.unpack_from(">4I", binary) == (0xfeedface, 18, 0, 1)
assert b"0.0.5d6\0" in binary and b"XenosRing: rptr=" in binary
assert b"XenosMarker: %s MEM_WRITE" in binary
assert b"XenosColorTriangle: %s 32x32 color=" in binary and b"XenosColorTrianglePixels" in binary
assert b"untouched=%lu unexpected=%lu" in binary and b"tolerance=%d" in binary
old_info = plistlib.loads(entries[target][1])
new_info = plistlib.loads(plist)
for key in ("CFBundleVersion", "OSBundleCompatibleVersion"):
    assert old_info.pop(key) == "0.0.5d5"
    assert new_info.pop(key) == "0.0.5d6"
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

elf = (base / "openbios-xbox360-gpu-triangle-retry.elf").read_bytes()
assert hashlib.md5(elf).hexdigest() == "1d002ad58e2d013336ff00810738c695"
name = b"XenosTR2.mkext\0"
triangle_name = b"XenosRGB.mkext\0"
assert len(name) == len(triangle_name) and elf.count(name) == 1 and triangle_name not in elf
test_elf = elf.replace(name, triangle_name)
assert test_elf.replace(triangle_name, name) == elf
elf_name = "openbios-xbox360-gpu-color-triangle.elf"

config = (base / "kboot-gpu-triangle-retry.conf").read_bytes()
assert hashlib.md5(config).hexdigest() == "0dee4bd74c96716933a173f68cccf586"
assert config.count(b"default=gputriretry\n") == 1 and config.endswith(b"\n")
config = config.replace(b"default=gputriretry\n", b"default=gpucolor\n")
config += f'gpucolor="uda0:/{elf_name}"\n'.encode()

stage = root / "boot-stage"
stage.mkdir(exist_ok=True)
files = {"XenosRGB.mkext": archive, elf_name: test_elf, "kboot-gpu-color-triangle.conf": config}
for name, data in files.items():
    (stage / name).write_bytes(data)
    print(f"{hashlib.md5(data).hexdigest()}  {len(data):>7}  {name}")
