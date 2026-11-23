import ast
import hashlib
from pathlib import Path
import plistlib
import struct
import zlib

root = Path(__file__).resolve().parent
base = root.parent / "gpu-compositor-context" / "boot-stage"
stage = root / "boot-stage"
codec_path = root.parent / "tx-build" / "repack.py"
codec_ast = ast.parse(codec_path.read_text())
codec_ast.body = [node for node in codec_ast.body if isinstance(node, ast.FunctionDef)]
codec = {"struct": struct, "zlib": zlib, "plistlib": plistlib}
exec(compile(codec_ast, str(codec_path), "exec"), codec)
archive_name = "XenosWCR.mkext"
firmware_name = "openbios-xbox360-gpu-compositor-retry.elf"
config_name = "kboot-gpu-compositor-retry.conf"


def verified_file(filename, digest):
    data = (base / filename).read_bytes()
    assert hashlib.md5(data).hexdigest() == digest
    return data


source = verified_file("XenosWCC.mkext", "1d13710c7771021b6d4bd50db57580e0")
entries = codec["read_archive"](source)
updates = {}
for name, personality in (
    ("XenonCoreGraphics", "XenosDisplayAccelerator"),
    ("XenonFramebufferTest", "XenonFramebufferTest"),
):
    identifier = "fish.goldfish64.osx360." + name
    _, original_plist, binary = entries[identifier]
    info = plistlib.loads(original_plist)
    props = info["IOKitPersonalities"][personality]
    assert props["IOGLBundleName"] == "GLDriverXenosCompositor"
    props["IOGLBundleName"] = "GLDriverXenosCompositorRetry"
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

firmware = verified_file("openbios-xbox360-gpu-compositor-context.elf", "7a6ed9d448391eb911a124abb4d4edd8")
old_name = b"XenosWCC.mkext\0"
new_name = archive_name.encode() + b"\0"
assert len(old_name) == len(new_name) and firmware.count(old_name) == 1 and new_name not in firmware
boot_config = verified_file("kboot-gpu-compositor-context.conf", "d4e5729d0e58cd7b12194d3469f59509")
assert boot_config.count(b"default=gpucompositor\n") == 1 and boot_config.endswith(b"\n")
boot_config = boot_config.replace(b"default=gpucompositor\n", b"default=gpucompretry\n")
boot_config += f'gpucompretry="uda0:/{firmware_name}"\n'.encode()

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
