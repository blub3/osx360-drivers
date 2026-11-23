import ast
import hashlib
from pathlib import Path
import plistlib
import struct
import zlib

root = Path(__file__).resolve().parent
base = root.parent / "gpu-display-surface" / "boot-stage"
stage = root / "boot-stage"
codec_path = root.parent / "tx-build" / "repack.py"
codec_ast = ast.parse(codec_path.read_text())
codec_ast.body = [node for node in codec_ast.body if isinstance(node, ast.FunctionDef)]
codec = {"struct": struct, "zlib": zlib, "plistlib": plistlib}
exec(compile(codec_ast, str(codec_path), "exec"), codec)
driver = "fish.goldfish64.osx360.XenonCoreGraphics"
framebuffer = "fish.goldfish64.osx360.XenonFramebufferTest"
bundle_name = "GLDriverXenosWSProbe"
archive_name = "XenosWSP.mkext"
firmware_name = "openbios-xbox360-gpu-windowserver-submit.elf"


def verified_file(filename, digest):
    data = (base / filename).read_bytes()
    assert hashlib.md5(data).hexdigest() == digest
    return data


source = verified_file("XenosWSS.mkext", "c717c0c0606b19b4eb13b48a403e107f")
entries = codec["read_archive"](source)
_, old_plist, _ = entries[driver]
info = plistlib.loads(old_plist)
for key in ("CFBundleVersion", "OSBundleCompatibleVersion"):
    assert info[key] == "0.0.5d42"
    info[key] = "0.0.5d43"
libraries = info["OSBundleLibraries"]
assert libraries["com.apple.kpi.bsd"] == "8.11.0"
assert libraries["com.apple.kpi.mach"] == "8.11.0"
info["IOKitPersonalities"]["XenosDisplayAccelerator"]["IOGLBundleName"] = bundle_name
plist = plistlib.dumps(info)
binary = (root / "XenonCoreGraphics-linked").read_bytes()
assert struct.unpack_from(">4I", binary) == (0xfeedface, 18, 0, 1)
module = driver.encode().ljust(64, b"\0")
assert binary.count(module + b"0.0.5d43".ljust(64, b"\0")) == 1
assert b"XenosDisplaySurfaceClient\0" in binary

_, framebuffer_plist, framebuffer_binary = entries[framebuffer]
framebuffer_info = plistlib.loads(framebuffer_plist)
framebuffer_info["IOKitPersonalities"]["XenonFramebufferTest"]["IOGLBundleName"] = bundle_name
framebuffer_plist = plistlib.dumps(framebuffer_info)
updates = {
    driver: (plist, binary),
    framebuffer: (framebuffer_plist, framebuffer_binary),
}

archive = bytearray(source[:32 + 32 * len(entries)])
for identifier, (row, *_payloads) in entries.items():
    if identifier == driver:
        codec["append_payload"](archive, row, plist)
        codec["append_payload"](archive, row + 16, binary)
        continue
    if identifier == framebuffer:
        codec["append_payload"](archive, row, framebuffer_plist)
    slots = (row + 16,) if identifier == framebuffer else (row, row + 16)
    for slot in slots:
        offset, compressed, length, _ = struct.unpack_from(">4I", source, slot)
        archive.extend(bytes(-len(archive) % 32))
        struct.pack_into(">I", archive, slot, len(archive))
        archive.extend(source[offset:offset + (compressed or length)])
archive.extend(bytes(-len(archive) % 32))
struct.pack_into(">II", archive, 8, len(archive), zlib.adler32(archive[16:]))
decoded = codec["read_archive"](archive)
assert decoded.keys() == entries.keys()
for identifier, (_, decoded_plist, decoded_binary) in decoded.items():
    assert (decoded_plist, decoded_binary) == updates.get(identifier, entries[identifier][1:])

firmware = verified_file("openbios-xbox360-gpu-display-surface.elf", "6e6f5ec3ac7e547f4146f61b2cd05406")
old_name = b"XenosWSS.mkext\0"
new_name = archive_name.encode() + b"\0"
assert len(old_name) == len(new_name) and firmware.count(old_name) == 1 and new_name not in firmware
boot_config = verified_file("kboot-gpu-display-surface.conf", "5790ac8b037422bf101f57fd9e51c55b")
assert boot_config.count(b"default=gpudisplaysurface\n") == 1 and boot_config.endswith(b"\n")
boot_config = boot_config.replace(b"default=gpudisplaysurface\n", b"default=gpuwssubmit\n")
boot_config += f'gpuwssubmit="uda0:/{firmware_name}"\n'.encode()

for filename, payload in (
    (archive_name, archive),
    (firmware_name, firmware.replace(old_name, new_name)),
    ("kboot-gpu-windowserver-submit.conf", boot_config),
):
    (stage / filename).write_bytes(payload)

for identifier, name in ((driver, "XenonCoreGraphics"), (framebuffer, "XenonFramebufferTest")):
    contents = root / "link-preflight" / "native" / (name + ".kext") / "Contents"
    (contents / "MacOS").mkdir(parents=True, exist_ok=True)
    (contents / "Info.plist").write_bytes(decoded[identifier][1])
    (contents / "MacOS" / name).write_bytes(decoded[identifier][2])
