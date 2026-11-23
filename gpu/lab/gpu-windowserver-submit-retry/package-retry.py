import ast
import hashlib
from pathlib import Path
import plistlib
import struct
import zlib

root = Path(__file__).resolve().parent
base = root.parent / "gpu-windowserver-submit" / "boot-stage"
stage = root / "boot-stage"
codec_path = root.parent / "tx-build" / "repack.py"
codec_ast = ast.parse(codec_path.read_text())
codec_ast.body = [node for node in codec_ast.body if isinstance(node, ast.FunctionDef)]
codec = {"struct": struct, "zlib": zlib, "plistlib": plistlib}
exec(compile(codec_ast, str(codec_path), "exec"), codec)
archive_name = "XenosWFR.mkext"
firmware_name = "openbios-xbox360-gpu-windowserver-framebuffer.elf"
config_name = "kboot-gpu-windowserver-framebuffer.conf"


def verified_file(filename, digest):
    data = (base / filename).read_bytes()
    assert hashlib.md5(data).hexdigest() == digest
    return data


source = verified_file("XenosWSP.mkext", "18663e43a5af0009a381b562cb5ca66d")
entries = codec["read_archive"](source)
updates = {}
for name, personality, old_version, new_version in (
    ("XenonCoreGraphics", "XenosDisplayAccelerator", "0.0.5d43", "0.0.5d44"),
    ("XenonFramebufferTest", "XenonFramebufferTest", "0.0.5d1", "0.0.5d2"),
):
    identifier = "fish.goldfish64.osx360." + name
    _, original_plist, _ = entries[identifier]
    info = plistlib.loads(original_plist)
    for key in ("CFBundleVersion", "OSBundleCompatibleVersion"):
        if key in info:
            assert info[key] == old_version
            info[key] = new_version
    props = info["IOKitPersonalities"][personality]
    assert props["IOGLBundleName"] == "GLDriverXenosWSProbe"
    props["IOGLBundleName"] = "GLDriverXenosNative"
    libraries = info["OSBundleLibraries"]
    if name == "XenonFramebufferTest":
        assert "com.apple.kpi.bsd" not in libraries and "com.apple.kpi.mach" not in libraries
        libraries.update({"com.apple.kpi.bsd": "8.11.0", "com.apple.kpi.mach": "8.11.0"})
    else:
        assert libraries["com.apple.kpi.bsd"] == libraries["com.apple.kpi.mach"] == "8.11.0"
    binary = (root / f"{name}-linked").read_bytes()
    assert struct.unpack_from(">4I", binary) == (0xfeedface, 18, 0, 1)
    module = identifier.encode().ljust(64, b"\0")
    assert binary.count(module + new_version.encode().ljust(64, b"\0")) == 1
    assert b"XenosWindowServerFramebufferOpened\0" in binary
    updates[identifier] = plistlib.dumps(info), binary

archive = bytearray(source[:32 + 32 * len(entries)])
for identifier, (row, _, _) in entries.items():
    if identifier in updates:
        plist, binary = updates[identifier]
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
for identifier, (_, plist, binary) in decoded.items():
    assert (plist, binary) == updates.get(identifier, entries[identifier][1:])

firmware = verified_file("openbios-xbox360-gpu-windowserver-submit.elf", "746b948434ee1eb0f6a56876cb452f8c")
old_name = b"XenosWSP.mkext\0"
new_name = archive_name.encode() + b"\0"
assert len(old_name) == len(new_name) and firmware.count(old_name) == 1 and new_name not in firmware
boot_config = verified_file("kboot-gpu-windowserver-submit.conf", "6d5c258b4b5adb7af7f78c35c932bff9")
assert boot_config.count(b"default=gpuwssubmit\n") == 1 and boot_config.endswith(b"\n")
boot_config = boot_config.replace(b"default=gpuwssubmit\n", b"default=gpuwsfbretry\n")
boot_config += f'gpuwsfbretry="uda0:/{firmware_name}"\n'.encode()

for filename, payload in (
    (archive_name, archive),
    (firmware_name, firmware.replace(old_name, new_name)),
    (config_name, boot_config),
):
    (stage / filename).write_bytes(payload)

for identifier in updates:
    name = identifier.rsplit(".", 1)[-1]
    contents = root / "link-preflight" / (name + ".kext") / "Contents"
    (contents / "MacOS").mkdir(parents=True, exist_ok=True)
    (contents / "Info.plist").write_bytes(decoded[identifier][1])
    (contents / "MacOS" / name).write_bytes(decoded[identifier][2])
