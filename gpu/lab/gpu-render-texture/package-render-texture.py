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

base = root.parent / "gpu-indexed-quad/boot-stage"
original = (base / "XenosIDX.mkext").read_bytes()
assert hashlib.md5(original).hexdigest() == "a9c3d97947e415f9c5b6591c57422f51"
entries = codec["read_archive"](original)
archive = bytearray(original[:32 + 32 * len(entries)])
target = "fish.goldfish64.osx360.XenonCoreGraphics"
plist = (root / "source/XenonCoreGraphics/build_tiger/XenonCoreGraphics-Info.plist").read_bytes()
binary = (root / "XenonCoreGraphics-linked").read_bytes()
assert struct.unpack_from(">4I", binary) == (0xfeedface, 18, 0, 1)
assert b"0.0.5d11\0" in binary and b"XenosRing: rptr=" in binary
assert b"XenosMarker: %s MEM_WRITE" in binary
assert b"XenosRenderTexture: %s 32x32 passes=" in binary and b"XenosRenderTextureSourcePixels" in binary
assert b"XenosRenderTextureQuadPixels" in binary and b"sample_mismatches=%lu" in binary
old_info = plistlib.loads(entries[target][1])
new_info = plistlib.loads(plist)
for key in ("CFBundleVersion", "OSBundleCompatibleVersion"):
    assert old_info.pop(key) == "0.0.5d10"
    assert new_info.pop(key) == "0.0.5d11"
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

elf = (base / "openbios-xbox360-gpu-indexed-quad.elf").read_bytes()
assert hashlib.md5(elf).hexdigest() == "115202953caf08bbf491f79ab153a2f0"
name = b"XenosIDX.mkext\0"
triangle_name = b"XenosRTT.mkext\0"
assert len(name) == len(triangle_name) and elf.count(name) == 1 and triangle_name not in elf
test_elf = elf.replace(name, triangle_name)
assert test_elf.replace(triangle_name, name) == elf
elf_name = "openbios-xbox360-gpu-render-texture.elf"

config = (base / "kboot-gpu-indexed-quad.conf").read_bytes()
assert hashlib.md5(config).hexdigest() == "301c4fdb67d1c5055c1cbc4353d842df"
assert config.count(b"default=gpuindex\n") == 1 and config.endswith(b"\n")
config = config.replace(b"default=gpuindex\n", b"default=gpurttex\n")
config += f'gpurttex="uda0:/{elf_name}"\n'.encode()

stage = root / "boot-stage"
stage.mkdir(exist_ok=True)
files = {"XenosRTT.mkext": archive, elf_name: test_elf, "kboot-gpu-render-texture.conf": config}
for name, data in files.items():
    (stage / name).write_bytes(data)
    print(f"{hashlib.md5(data).hexdigest()}  {len(data):>7}  {name}")
