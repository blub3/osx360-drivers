import ast
import hashlib
from pathlib import Path
import plistlib
import struct
import zlib

root = Path(__file__).resolve().parent
base = root.parent / "gpu-windowserver-retry" / "boot-stage"
stage = root / "boot-stage"
codec_path = root.parent / "tx-build" / "repack.py"
codec_ast = ast.parse(codec_path.read_text())
codec_ast.body = [node for node in codec_ast.body if isinstance(node, ast.FunctionDef)]
codec = {"struct": struct, "zlib": zlib, "plistlib": plistlib}
exec(compile(codec_ast, str(codec_path), "exec"), codec)

archive_name = "XenosGLD.mkext"
elf_name = "openbios-xbox360-gpu-native-gld.elf"
config_name = "kboot-gpu-native-gld.conf"
driver = "fish.goldfish64.osx360.XenonCoreGraphics"
framebuffer = "fish.goldfish64.osx360.XenonFramebufferTest"
bundle_name = "GLDriverXenosNative"


def verified_file(filename, digest):
    data = (base / filename).read_bytes()
    assert hashlib.md5(data).hexdigest() == digest
    return data


source = verified_file("XenosQER.mkext", "4ef8f13100a98b993d9896cd709f4e38")
entries = codec["read_archive"](source)
updated = {}
for identifier, personality in (
    (driver, "XenosDisplayAccelerator"),
    (framebuffer, "XenonFramebufferTest"),
):
    _, plist, binary = entries[identifier]
    info = plistlib.loads(plist)
    properties = info["IOKitPersonalities"][personality]
    assert "IOGLBundleName" not in properties
    properties["IOGLBundleName"] = bundle_name
    if identifier == driver:
        for key in ("CFBundleVersion", "OSBundleCompatibleVersion"):
            assert info[key] == "0.0.5d40"
            info[key] = "0.0.5d41"
        module = driver.encode().ljust(64, b"\0")
        old = module + b"0.0.5d40".ljust(64, b"\0")
        new = module + b"0.0.5d41".ljust(64, b"\0")
        assert binary.count(old) == 1 and new not in binary
        binary = binary.replace(old, new)
    updated[identifier] = plistlib.dumps(info), binary

archive = bytearray(source[:32 + 32 * len(entries)])
for identifier, (row, _, _) in entries.items():
    for index in range(2):
        slot = row + index * 16
        replacement = updated.get(identifier)
        if replacement and (index == 0 or identifier == driver):
            codec["append_payload"](archive, slot, replacement[index])
        else:
            offset, compressed, length, _ = struct.unpack_from(">4I", source, slot)
            archive.extend(bytes(-len(archive) % 32))
            struct.pack_into(">I", archive, slot, len(archive))
            archive.extend(source[offset:offset + (compressed or length)])
archive.extend(bytes(-len(archive) % 32))
struct.pack_into(">II", archive, 8, len(archive), zlib.adler32(archive[16:]))
decoded = codec["read_archive"](archive)
assert decoded.keys() == entries.keys()
for identifier, (_, plist, binary) in decoded.items():
    assert (plist, binary) == updated.get(identifier, entries[identifier][1:])

firmware = verified_file("openbios-xbox360-gpu-windowserver-retry.elf", "8ab1db30e4eea6c78d3d2cd5a723e442")
old_name = b"XenosQER.mkext\0"
new_name = archive_name.encode() + b"\0"
assert len(old_name) == len(new_name) and firmware.count(old_name) == 1 and new_name not in firmware
boot_config = verified_file("kboot-gpu-windowserver-retry.conf", "6f70043baec8ea1b5280136cf16856be")
assert boot_config.count(b"default=gpuxenoswsretry\n") == 1 and boot_config.endswith(b"\n")
boot_config = boot_config.replace(b"default=gpuxenoswsretry\n", b"default=gpunativegld\n")
boot_config += f'gpunativegld="uda0:/{elf_name}"\n'.encode()

for filename, payload in (
    (archive_name, archive),
    (elf_name, firmware.replace(old_name, new_name)),
    (config_name, boot_config),
):
    (stage / filename).write_bytes(payload)

for identifier, names in ((driver, "XenonCoreGraphics"), (framebuffer, "XenonFramebufferTest")):
    contents = root / "link-preflight" / "native" / (names + ".kext") / "Contents"
    (contents / "MacOS").mkdir(parents=True, exist_ok=True)
    (contents / "Info.plist").write_bytes(decoded[identifier][1])
    (contents / "MacOS" / names).write_bytes(decoded[identifier][2])
