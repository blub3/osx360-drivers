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

base = root.parent / "gpu-alpha-blend/boot-stage"
original = (base / "XenosALP.mkext").read_bytes()
assert hashlib.md5(original).hexdigest() == "29d4dc813afaafd780fe0eaf4d5a889f"
entries = codec["read_archive"](original)
archive = bytearray(original[:32 + 32 * len(entries)])
target = "fish.goldfish64.osx360.XenonCoreGraphics"
plist = (root / "source/XenonCoreGraphics/build_tiger/XenonCoreGraphics-Info.plist").read_bytes()
binary = (root / "XenonCoreGraphics-linked").read_bytes()
assert struct.unpack_from(">4I", binary) == (0xfeedface, 18, 0, 1)
assert b"0.0.5d10\0" in binary and b"XenosRing: rptr=" in binary
assert b"XenosMarker: %s MEM_WRITE" in binary
assert b"XenosIndexedQuad: %s 32x32 indices=" in binary and b"XenosIndexedQuadPixels" in binary
assert b"quad=%lu seam=%lu background=%lu" in binary
old_info = plistlib.loads(entries[target][1])
new_info = plistlib.loads(plist)
for key in ("CFBundleVersion", "OSBundleCompatibleVersion"):
    assert old_info.pop(key) == "0.0.5d9"
    assert new_info.pop(key) == "0.0.5d10"
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

elf = (base / "openbios-xbox360-gpu-alpha-blend.elf").read_bytes()
assert hashlib.md5(elf).hexdigest() == "5b1bdb110a83f384eae679fe1a5b9925"
name = b"XenosALP.mkext\0"
triangle_name = b"XenosIDX.mkext\0"
assert len(name) == len(triangle_name) and elf.count(name) == 1 and triangle_name not in elf
test_elf = elf.replace(name, triangle_name)
assert test_elf.replace(triangle_name, name) == elf
elf_name = "openbios-xbox360-gpu-indexed-quad.elf"

config = (base / "kboot-gpu-alpha-blend.conf").read_bytes()
assert hashlib.md5(config).hexdigest() == "4139f3ef00dab9ff0a70a006fa9d1b90"
assert config.count(b"default=gpublend\n") == 1 and config.endswith(b"\n")
config = config.replace(b"default=gpublend\n", b"default=gpuindex\n")
config += f'gpuindex="uda0:/{elf_name}"\n'.encode()

stage = root / "boot-stage"
stage.mkdir(exist_ok=True)
files = {"XenosIDX.mkext": archive, elf_name: test_elf, "kboot-gpu-indexed-quad.conf": config}
for name, data in files.items():
    (stage / name).write_bytes(data)
    print(f"{hashlib.md5(data).hexdigest()}  {len(data):>7}  {name}")
