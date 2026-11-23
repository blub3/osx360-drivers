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

base = root.parent / "native-hdmi/boot-stage"
original = (base / "NativeHD.mkext").read_bytes()
assert hashlib.md5(original).hexdigest() == "b99d4be9377e3e7965bb3e88d5f1ff0e"
entries = codec["read_archive"](original)
archive = bytearray(original[:32 + 32 * len(entries)])
target = "fish.goldfish64.osx360.XenonCoreGraphics"
plist = (root / "source/XenonCoreGraphics/build_tiger/XenonCoreGraphics-Info.plist").read_bytes()
binary = (root / "XenonCoreGraphics-linked").read_bytes()
assert struct.unpack_from(">4I", binary) == (0xfeedface, 18, 0, 1)
assert b"0.0.5d1\0" in binary and b"XenosRing: rptr=" in binary
old_info = plistlib.loads(entries[target][1])
new_info = plistlib.loads(plist)
for key in ("CFBundleVersion", "OSBundleCompatibleVersion"):
    assert old_info.pop(key) == "0.0.5"
    assert new_info.pop(key) == "0.0.5d1"
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

elf = (base / "openbios-xbox360-native-hdmi-test.elf").read_bytes()
assert hashlib.md5(elf).hexdigest() == "ca72c036347285e3fe77b58d3b2c93c6"
name = b"NativeHD.mkext\0"
ring_name = b"XenosGPU.mkext\0"
assert len(name) == len(ring_name) and elf.count(name) == 1 and ring_name not in elf
test_elf = elf.replace(name, ring_name)
assert test_elf.replace(ring_name, name) == elf
elf_name = "openbios-xbox360-gpu-ring-test.elf"

config = (base / "kboot-native-hdmi-test.conf").read_bytes()
assert hashlib.md5(config).hexdigest() == "7ee412f046fff91d1fec7713ebbf749b"
assert config.count(b"default=hdminative\n") == 1 and config.endswith(b"\n")
config = config.replace(b"default=hdminative\n", b"default=gpuring\n")
config += f'gpuring="uda0:/{elf_name}"\n'.encode()

stage = root / "boot-stage"
stage.mkdir(exist_ok=True)
files = {"XenosGPU.mkext": archive, elf_name: test_elf, "kboot-gpu-ring-test.conf": config}
for name, data in files.items():
    (stage / name).write_bytes(data)
    print(f"{hashlib.md5(data).hexdigest()}  {len(data):>7}  {name}")
