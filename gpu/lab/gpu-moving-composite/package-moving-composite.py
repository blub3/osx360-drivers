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

base = root.parent / "gpu-texture-composite/boot-stage"
original = (base / "XenosCMP.mkext").read_bytes()
assert hashlib.md5(original).hexdigest() == "dfa2e5d8d74dd053aee8bdc489e03aac"
entries = codec["read_archive"](original)
archive = bytearray(original[:32 + 32 * len(entries)])
target = "fish.goldfish64.osx360.XenonCoreGraphics"
plist = (root / "source/XenonCoreGraphics/build_tiger/XenonCoreGraphics-Info.plist").read_bytes()
binary = (root / "XenonCoreGraphics-linked").read_bytes()
assert struct.unpack_from(">4I", binary) == (0xfeedface, 18, 0, 1)
assert b"0.0.5d13\0" in binary and b"XenosRing: rptr=" in binary
assert b"XenosMarker: %s MEM_WRITE" in binary
assert b"XenosMovingComposite: %s %s checked=" in binary
assert b"XenosMovingCompositeResult" in binary and b"XenosMovingCompositeMovedFrames" in binary
assert b"XenosMovingCompositeLowAlphaProbes" in binary and b"XenosMovingCompositeHighAlphaProbes" in binary
old_info = plistlib.loads(entries[target][1])
new_info = plistlib.loads(plist)
for key in ("CFBundleVersion", "OSBundleCompatibleVersion"):
    assert old_info.pop(key) == "0.0.5d12"
    assert new_info.pop(key) == "0.0.5d13"
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

elf = (base / "openbios-xbox360-gpu-texture-composite.elf").read_bytes()
assert hashlib.md5(elf).hexdigest() == "8486de52ceca4b0eae62f2ee5bed7182"
name = b"XenosCMP.mkext\0"
demo_archive_name = b"XenosMOV.mkext\0"
assert len(name) == len(demo_archive_name) and elf.count(name) == 1 and demo_archive_name not in elf
test_elf = elf.replace(name, demo_archive_name)
assert test_elf.replace(demo_archive_name, name) == elf
elf_name = "openbios-xbox360-gpu-moving-composite.elf"

config = (base / "kboot-gpu-texture-composite.conf").read_bytes()
assert hashlib.md5(config).hexdigest() == "ddee0118e607e74df117d1698b089713"
assert config.count(b"default=gputexcomp\n") == 1 and config.endswith(b"\n")
config = config.replace(b"default=gputexcomp\n", b"default=gpumoving\n")
config += f'gpumoving="uda0:/{elf_name}"\n'.encode()

stage = root / "boot-stage"
stage.mkdir(exist_ok=True)
files = {"XenosMOV.mkext": archive, elf_name: test_elf, "kboot-gpu-moving-composite.conf": config}
for name, data in files.items():
    (stage / name).write_bytes(data)
    print(f"{hashlib.md5(data).hexdigest()}  {len(data):>7}  {name}")
