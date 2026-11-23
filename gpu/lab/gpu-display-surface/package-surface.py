import ast
import hashlib
from pathlib import Path
import plistlib
import struct
import zlib

root = Path(__file__).resolve().parent
base = root.parent / "gpu-native-gld" / "boot-stage"
stage = root / "boot-stage"
codec_path = root.parent / "tx-build" / "repack.py"
codec_ast = ast.parse(codec_path.read_text())
codec_ast.body = [node for node in codec_ast.body if isinstance(node, ast.FunctionDef)]
codec = {"struct": struct, "zlib": zlib, "plistlib": plistlib}
exec(compile(codec_ast, str(codec_path), "exec"), codec)
driver = "fish.goldfish64.osx360.XenonCoreGraphics"
archive_name = "XenosWSS.mkext"
firmware_name = "openbios-xbox360-gpu-display-surface.elf"


def verified_file(filename, digest):
    data = (base / filename).read_bytes()
    assert hashlib.md5(data).hexdigest() == digest
    return data


source = verified_file("XenosGLD.mkext", "cfc64a836626cfdef2793a0ad17f8da3")
entries = codec["read_archive"](source)
_, old_plist, _ = entries[driver]
info = plistlib.loads(old_plist)
for key in ("CFBundleVersion", "OSBundleCompatibleVersion"):
    assert info[key] == "0.0.5d41"
    info[key] = "0.0.5d42"
libraries = info["OSBundleLibraries"]
assert libraries["com.apple.kpi.bsd"] == "8.11.0"
assert "com.apple.kpi.mach" not in libraries
libraries["com.apple.kpi.mach"] = "8.11.0"
plist = plistlib.dumps(info)
binary = (root / "XenonCoreGraphics-linked").read_bytes()
assert struct.unpack_from(">4I", binary) == (0xfeedface, 18, 0, 1)
module = driver.encode().ljust(64, b"\0")
assert binary.count(module + b"0.0.5d42".ljust(64, b"\0")) == 1
assert b"XenosDisplaySurfaceClient\0" in binary

archive = bytearray(source[:32 + 32 * len(entries)])
for identifier, (row, *_payloads) in entries.items():
    if identifier == driver:
        codec["append_payload"](archive, row, plist)
        codec["append_payload"](archive, row + 16, binary)
        continue
    for slot in (row, row + 16):
        offset, compressed, length, _ = struct.unpack_from(">4I", source, slot)
        archive.extend(bytes(-len(archive) % 32))
        struct.pack_into(">I", archive, slot, len(archive))
        archive.extend(source[offset:offset + (compressed or length)])
archive.extend(bytes(-len(archive) % 32))
struct.pack_into(">II", archive, 8, len(archive), zlib.adler32(archive[16:]))
decoded = codec["read_archive"](archive)
assert decoded.keys() == entries.keys()
for identifier, (_, decoded_plist, decoded_binary) in decoded.items():
    assert (decoded_plist, decoded_binary) == ((plist, binary) if identifier == driver else entries[identifier][1:])

firmware = verified_file("openbios-xbox360-gpu-native-gld.elf", "d02059c9886d38f103e85f382bab3b72")
old_name = b"XenosGLD.mkext\0"
new_name = archive_name.encode() + b"\0"
assert len(old_name) == len(new_name) and firmware.count(old_name) == 1 and new_name not in firmware
boot_config = verified_file("kboot-gpu-native-gld.conf", "228691583a418df3e68ce58b5c8adca9")
assert boot_config.count(b"default=gpunativegld\n") == 1 and boot_config.endswith(b"\n")
boot_config = boot_config.replace(b"default=gpunativegld\n", b"default=gpudisplaysurface\n")
boot_config += f'gpudisplaysurface="uda0:/{firmware_name}"\n'.encode()

for filename, payload in (
    (archive_name, archive),
    (firmware_name, firmware.replace(old_name, new_name)),
    ("kboot-gpu-display-surface.conf", boot_config),
):
    (stage / filename).write_bytes(payload)

contents = root / "link-preflight" / "display-surface" / "XenonCoreGraphics.kext" / "Contents"
(contents / "MacOS").mkdir(parents=True, exist_ok=True)
(contents / "Info.plist").write_bytes(decoded[driver][1])
(contents / "MacOS" / "XenonCoreGraphics").write_bytes(decoded[driver][2])
