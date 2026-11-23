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

base = root.parent / "gpu-vblank-flips/boot-stage"
original = (base / "XenosVBL.mkext").read_bytes()
assert hashlib.md5(original).hexdigest() == "ac268db7d875f911ab59873cc4a8c971"
entries = codec["read_archive"](original)
archive = bytearray(original[:32 + 32 * len(entries)])
target = "fish.goldfish64.osx360.XenonCoreGraphics"
plist = (root / "source/XenonCoreGraphics/build_tiger/XenonCoreGraphics-Info.plist").read_bytes()
binary = (root / "XenonCoreGraphics-linked").read_bytes()
assert struct.unpack_from(">4I", binary) == (0xfeedface, 18, 0, 1)
assert b"0.0.5d15\0" in binary and b"XenosRing: rptr=" in binary
assert b"XenosMarker: %s MEM_WRITE" in binary
assert b"XenosVBlankFlip: %s %s checked=" in binary
assert b"XenosVBlankFlipResult" in binary and b"XenosVBlankFlipOrderErrors" in binary
assert b"XenosVBlankFlipLowAlphaProbes" in binary and b"XenosVBlankFlipHighAlphaProbes" in binary
assert b"XenosVBlankFlipLate" in binary and b"XenosVBlankFlipLatchErrors" in binary
assert b"XenosVBlankFlipDesktopRestored" in binary
assert b"xbox360-vblank-scanout\0" in binary
assert b"XenosVBlankFlipReservedBytes" in binary
old_info = plistlib.loads(entries[target][1])
new_info = plistlib.loads(plist)
for key in ("CFBundleVersion", "OSBundleCompatibleVersion"):
    assert old_info.pop(key) == "0.0.5d14"
    assert new_info.pop(key) == "0.0.5d15"
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

elf_name = "openbios-xbox360-gpu-vblank-retry.elf"
test_elf = (root / elf_name).read_bytes()
assert test_elf[:7] == b"\x7fELF\x01\x02\x01"
assert struct.unpack_from(">HH", test_elf, 16) == (2, 20)
assert test_elf.count(b"XenosVBR.mkext\0") == 1
assert b"XenosVBL.mkext\0" not in test_elf
assert b"xbox360-vblank-scanout\0" in test_elf
assert struct.pack(">II", 0x1e800000, 0x00800000) in test_elf
for marker in (b"xenon-hdmi-audio\0", b"xbox360-hdmi-audio-test\0",
               b"HDMI audio: %s", b"transmitter setup complete; HDMI 0214 wrote 11",
               b"programmed experimental 1080p60\0", b"xbox360-native-hdmi"):
    assert marker in test_elf, marker

config = (base / "kboot-gpu-vblank-flips.conf").read_bytes()
assert hashlib.md5(config).hexdigest() == "329aec17e3f504dbb463e6aca209b6f4"
assert config.count(b"default=gpuvblank\n") == 1 and config.endswith(b"\n")
config = config.replace(b"default=gpuvblank\n", b"default=gpuvbretry\n")
config += f'gpuvbretry="uda0:/{elf_name}"\n'.encode()

stage = root / "boot-stage"
stage.mkdir(exist_ok=True)
files = {"XenosVBR.mkext": archive, elf_name: test_elf, "kboot-gpu-vblank-retry.conf": config}
for name, data in files.items():
    (stage / name).write_bytes(data)
    print(f"{hashlib.md5(data).hexdigest()}  {len(data):>7}  {name}")
