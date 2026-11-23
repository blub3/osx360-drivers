import ast
import hashlib
from pathlib import Path
import plistlib
import struct
import zlib

root = Path(__file__).resolve().parent
stage = root / "boot-stage"
base = root.parent / "gpu-multi-client/boot-stage"
codec_path = root.parent / "tx-build/repack.py"
codec_ast = ast.parse(codec_path.read_text())
codec_ast.body = [node for node in codec_ast.body if isinstance(node, ast.FunctionDef)]
codec = {"struct": struct, "zlib": zlib, "plistlib": plistlib}
exec(compile(codec_ast, str(codec_path), "exec"), codec)

archive_name = "XenosCGL.mkext"
elf_name = "openbios-xbox360-gpu-cgl-offscreen.elf"
config_name = "kboot-gpu-cgl-offscreen.conf"
test_dir = "XenosCGLOffscreenTest"
target = "fish.goldfish64.osx360.XenonCoreGraphics"
old_archive_name = b"XenosMCL.mkext\0"
source_archive = (base / "XenosMCL.mkext").read_bytes()
assert hashlib.md5(source_archive).hexdigest() == "6e1ccd6bc9872628ac7dd00d70238720"
old_entries = codec["read_archive"](source_archive)
plist = (root / "source/XenonCoreGraphics/build_tiger/XenonCoreGraphics-Info.plist").read_bytes()
binary = (root / "XenonCoreGraphics-linked").read_bytes()
assert struct.unpack_from(">4I", binary) == (0xfeedface, 18, 0, 1)
assert all(token in binary for token in (b"0.0.5d38\0", b"queueQuadCapture", b"readQuadCapture",
           b"XenosGeometryQuad: %s ticket=", b"XenosSurfaceQueue: retired ticket="))
old_info, new_info = plistlib.loads(old_entries[target][1]), plistlib.loads(plist)
for key in ("CFBundleVersion", "OSBundleCompatibleVersion"):
    assert old_info.pop(key) == "0.0.5d37"
    assert new_info.pop(key) == "0.0.5d38"
assert old_info == new_info

archive = bytearray(source_archive[:32 + 32 * len(old_entries)])
for identifier, (row, _, _) in old_entries.items():
    if identifier == target:
        codec["append_payload"](archive, row, plist)
        codec["append_payload"](archive, row + 16, binary)
        continue
    for slot in (row, row + 16):
        offset, compressed, length, _ = struct.unpack_from(">4I", source_archive, slot)
        archive.extend(bytes(-len(archive) % 32))
        struct.pack_into(">I", archive, slot, len(archive))
        archive.extend(source_archive[offset:offset + (compressed or length)])
archive.extend(bytes(-len(archive) % 32))
struct.pack_into(">II", archive, 8, len(archive), zlib.adler32(archive[16:]))
new_entries = codec["read_archive"](archive)
assert new_entries.keys() == old_entries.keys()
for identifier, (_, info, executable) in new_entries.items():
    expected = (plist, binary) if identifier == target else old_entries[identifier][1:]
    assert (info, executable) == expected, identifier

source_elf = (base / "openbios-xbox360-gpu-multi-client.elf").read_bytes()
assert hashlib.md5(source_elf).hexdigest() == "0a4084be7471fe8fdcb54fceae45b8ef"
new_archive_name = archive_name.encode() + b"\0"
assert len(new_archive_name) == len(old_archive_name)
assert source_elf.count(old_archive_name) == 1 and new_archive_name not in source_elf
elf = source_elf.replace(old_archive_name, new_archive_name)
assert elf.replace(new_archive_name, old_archive_name) == source_elf
config = (base / "kboot-gpu-multi-client.conf").read_bytes()
assert hashlib.md5(config).hexdigest() == "2da466ec6ff69691f4c8cfa3ce29cf3f"
assert config.count(b"default=gpumulticlient\n") == 1 and config.endswith(b"\n")
assert b"gpucgloffscreen" not in config
config = config.replace(b"default=gpumulticlient\n", b"default=gpucgloffscreen\n")
config += f'gpucgloffscreen="uda0:/{elf_name}"\n'.encode()

app = (root / "build/XenosCGLOffscreen").read_bytes()
plugin = (root / "build/GLEngineXenos").read_bytes()
runner = (root / "RunXenosCGLOffscreen.sh").read_bytes()
assert struct.unpack_from(">4I", app) == (0xfeedface, 18, 0, 2)
assert struct.unpack_from(">4I", plugin) == (0xfeedface, 18, 0, 8)
assert all(name in app for name in (b"CGLSetOffScreen\0", b"glReadPixels\0", b"glFinish\0"))
assert all(name in plugin for name in (b"_gliCreateContext\0", b"_gliAttachDrawableWithOptions\0",
                                      b"IOConnectMethodStructureIStructureO\0"))
files = {archive_name: archive, elf_name: elf, config_name: config,
         f"{test_dir}/XenosCGLOffscreen": app, f"{test_dir}/GLEngineXenos": plugin,
         f"{test_dir}/RunXenosCGLOffscreen.sh": runner}
for name, data in files.items():
    path = stage / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    path.chmod(0o755 if name.startswith(f"{test_dir}/") else 0o644)
    print(f"{hashlib.md5(data).hexdigest()}  {name}")
