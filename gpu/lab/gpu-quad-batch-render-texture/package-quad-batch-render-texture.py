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

base = root.parent / "gpu-quad-batch-depth/boot-stage"
original = (base / "XenosQDP.mkext").read_bytes()
assert hashlib.md5(original).hexdigest() == "a1c74314223a3133765a57e44dfd7329"
entries = codec["read_archive"](original)
archive = bytearray(original[:32 + 32 * len(entries)])
target = "fish.goldfish64.osx360.XenonCoreGraphics"
plist = (root / "source/XenonCoreGraphics/build_tiger/XenonCoreGraphics-Info.plist").read_bytes()
binary = (root / "XenonCoreGraphics-linked").read_bytes()
assert struct.unpack_from(">4I", binary) == (0xfeedface, 18, 0, 1)
assert b"0.0.5d28\0" in binary and b"XenosRing: rptr=" in binary
assert b"XenosMarker: %s MEM_WRITE" in binary
assert b"XenosQuadClient\0" in binary
assert b"XenosUserQuad: %s ticket=" in binary
assert b"XenosBatchRenderTexture: %s ticket=" in binary
assert b"XenosBatchDepth: %s ticket=" in binary
assert b"XenosBatchScissorStencil: %s ticket=" in binary
assert b"XenosBatchStencil: %s ticket=" in binary
assert b"XenosBatchScissor: %s ticket=" in binary
assert b"XenosBatchBlend: %s ticket=" in binary
assert b"XenosBatchQuad: %s ticket=" in binary
assert b"XenosRotatedQuad: %s ticket=" in binary
assert b"XenosGeometryQuad: %s ticket=" in binary
assert b"XenosUploadQuad: %s ticket=" in binary and b"XenosUserQuadReadbackHash" in binary
assert all(marker in binary for marker in (
    b"XenosUserQuadResult", b"XenosUserQuadFence", b"XenosUserQuadPixelsChecked",
    b"XenosUserQuadMismatches", b"IOUserClientClass"))
app = (root / "XenosQuadBatchRenderTexture").read_bytes()
assert struct.unpack_from(">4I", app) == (0xfeedface, 18, 0, 2)
assert b"XenosQuadBatchRenderTexture 1\0" in app and b"_IOConnectMethodScalarIScalarO\0" in app
assert b"_IOConnectMethodStructureIStructureO\0" in app
old_info = plistlib.loads(entries[target][1])
new_info = plistlib.loads(plist)
for key in ("CFBundleVersion", "OSBundleCompatibleVersion"):
    assert old_info.pop(key) == "0.0.5d27"
    assert new_info.pop(key) == "0.0.5d28"
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

elf_name = "openbios-xbox360-gpu-quad-batch-render-texture.elf"
working_firmware = root.parent / "native-hdmi/boot-stage/openbios-xbox360-native-hdmi-test.elf"
base_elf = working_firmware.read_bytes()
assert hashlib.md5(base_elf).hexdigest() == "ca72c036347285e3fe77b58d3b2c93c6"
assert base_elf[:7] == b"\x7fELF\x01\x02\x01" and struct.unpack_from(">HH", base_elf, 16) == (2, 20)
previous_archive = b"NativeHD.mkext\0"
archive_name = b"XenosQRT.mkext\0"
assert len(previous_archive) == len(archive_name) and base_elf.count(previous_archive) == 1
assert archive_name not in base_elf
test_elf = base_elf.replace(previous_archive, archive_name)
assert test_elf.replace(archive_name, previous_archive) == base_elf

config = (base / "kboot-gpu-quad-batch-depth.conf").read_bytes()
assert hashlib.md5(config).hexdigest() == "fbfdaf0c08e94e6c9ae2d66503395ff2"
assert config.count(b"default=gpuquaddepth\n") == 1 and config.endswith(b"\n")
config = config.replace(b"default=gpuquaddepth\n", b"default=gpuquadrtt\n")
config += f'gpuquadrtt="uda0:/{elf_name}"\n'.encode()

stage = root / "boot-stage"
stage.mkdir(exist_ok=True)
files = {"XenosQRT.mkext": archive, elf_name: test_elf, "kboot-gpu-quad-batch-render-texture.conf": config, "XenosQuadBatchRenderTexture": app}
for name, data in files.items():
    (stage / name).write_bytes(data)
    print(f"{hashlib.md5(data).hexdigest()}  {len(data):>7}  {name}")
