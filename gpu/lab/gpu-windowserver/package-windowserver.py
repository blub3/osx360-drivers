import ast
import hashlib
from pathlib import Path
import plistlib
import struct
import zlib

root = Path(__file__).resolve().parent
stage = root / "boot-stage"
base = root.parent / "gpu-cgl-offscreen/boot-stage"
codec_path = root.parent / "tx-build/repack.py"
codec_ast = ast.parse(codec_path.read_text())
codec_ast.body = [node for node in codec_ast.body if isinstance(node, ast.FunctionDef)]
codec = {"struct": struct, "zlib": zlib, "plistlib": plistlib}
exec(compile(codec_ast, str(codec_path), "exec"), codec)

archive_name = "XenosQES.mkext"
elf_name = "openbios-xbox360-gpu-windowserver.elf"
config_name = "kboot-gpu-windowserver.conf"
test_dir = "XenosWindowServerTest"
target = "fish.goldfish64.osx360.XenonCoreGraphics"
old_archive_name = b"XenosCGL.mkext\0"
source_archive = (base / "XenosCGL.mkext").read_bytes()
assert hashlib.md5(source_archive).hexdigest() == "8ac34c28a1cc94f3ccd7bf9be6eeaa64"
old_entries = codec["read_archive"](source_archive)
plist = (root / "source/XenonCoreGraphics/build_tiger/XenonCoreGraphics-Info.plist").read_bytes()
binary = (root / "XenonCoreGraphics-linked").read_bytes()
assert struct.unpack_from(">4I", binary) == (0xfeedface, 18, 0, 1)
assert all(token in binary for token in (b"0.0.5d39\0", b"queueQuadCapture", b"readQuadCapture",
           b"XenosGeometryQuad: %s ticket=", b"XenosSurfaceQueue: retired ticket="))
old_info, new_info = plistlib.loads(old_entries[target][1]), plistlib.loads(plist)
for key in ("CFBundleVersion", "OSBundleCompatibleVersion"):
    assert old_info.pop(key) == "0.0.5d38"
    assert new_info.pop(key) == "0.0.5d39"
assert new_info["OSBundleLibraries"].pop("com.apple.iokit.IOGraphicsFamily") == "1.4"
native = new_info["IOKitPersonalities"].pop("XenosDisplayAccelerator")
assert native["IOClass"] == "XenosDisplayAccelerator"
assert native["IOPCIMatch"] == "0x58411414" and native["IOProviderClass"] == "IOPCIDevice"
assert old_info == new_info
assert all(marker in binary for marker in (b"XenosDisplayAccelerator", b"XenosGPUCompletion",
         b"AccelCaps", b"IOAccelTypes", b"_proc_selfpid"))

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

source_elf = (base / "openbios-xbox360-gpu-cgl-offscreen.elf").read_bytes()
assert hashlib.md5(source_elf).hexdigest() == "e2e17593c6a08531285d5c3fd2604837"
new_archive_name = archive_name.encode() + b"\0"
assert len(new_archive_name) == len(old_archive_name)
assert source_elf.count(old_archive_name) == 1 and new_archive_name not in source_elf
elf = source_elf.replace(old_archive_name, new_archive_name)
assert elf.replace(new_archive_name, old_archive_name) == source_elf
config = (base / "kboot-gpu-cgl-offscreen.conf").read_bytes()
assert hashlib.md5(config).hexdigest() == "2a15e64f7e185e0790d5780b578fb016"
assert config.count(b"default=gpucgloffscreen\n") == 1 and config.endswith(b"\n")
assert b"gpuxenosws" not in config
config = config.replace(b"default=gpucgloffscreen\n", b"default=gpuxenosws\n")
config += f'gpuxenosws="uda0:/{elf_name}"\n'.encode()

app = (root / "build/XenosWindowServerCheck").read_bytes()
runner = (root / "RunXenosWindowServer.sh").read_bytes()
notes = (root / "TestNotes.txt").read_bytes()
assert struct.unpack_from(">4I", app) == (0xfeedface, 18, 0, 2)
assert all(name in app for name in (b"CGDisplayUsesOpenGLAcceleration\0",
           b"IOAccelFindAccelerator\0", b"XenosGPUCompletion\0", b"MoveWindow\0"))
files = {archive_name: archive, elf_name: elf, config_name: config,
         f"{test_dir}/XenosWindowServerCheck": app,
         f"{test_dir}/RunXenosWindowServer.sh": runner,
         f"{test_dir}/TestNotes.txt": notes}
for name, data in files.items():
    path = stage / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    path.chmod(0o755 if name.endswith(("XenosWindowServerCheck", ".sh")) else 0o644)
    print(f"{hashlib.md5(data).hexdigest()}  {name}")
