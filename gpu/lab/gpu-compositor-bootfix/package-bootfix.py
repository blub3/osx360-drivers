import ast
import hashlib
from pathlib import Path
import plistlib
import re
import struct
import zlib

root = Path(__file__).resolve().parent
working = root.parent / "gpu-compositor-context" / "boot-stage"
retry = root.parent / "gpu-compositor-retry" / "boot-stage"
stage = root / "boot-stage"
stage.mkdir(exist_ok=True)

codec_file = root.parent / "tx-build" / "repack.py"
codec_ast = ast.parse(codec_file.read_text())
codec_ast.body = [node for node in codec_ast.body if isinstance(node, ast.FunctionDef)]
codec = {"struct": struct, "zlib": zlib, "plistlib": plistlib}
exec(compile(codec_ast, str(codec_file), "exec"), codec)


def verified(path, digest):
    data = path.read_bytes()
    if hashlib.md5(data).hexdigest() != digest:
        raise ValueError(f"Checksum mismatch for {path}")
    return data


def compact_xml(payload):
    # These are generated kext plists. Trim indentation between tags only.
    compact = re.sub(rb">[\t \n\r]+<", b"><", payload)
    if plistlib.loads(compact) != plistlib.loads(payload):
        raise ValueError("Compacting the plist changed its values")
    return compact


old_archive = verified(working / "XenosWCC.mkext", "1d13710c7771021b6d4bd50db57580e0")
retry_archive = verified(retry / "XenosWCR.mkext", "cf2b0b281476ebd922e9d07dc979c7e0")
original = codec["read_archive"](old_archive)
expected = codec["read_archive"](retry_archive)
assert original.keys() == expected.keys()
archive = bytearray(old_archive)
changed = {
    "fish.goldfish64.osx360.XenonCoreGraphics": "XenosDisplayAccelerator",
    "fish.goldfish64.osx360.XenonFramebufferTest": "XenonFramebufferTest",
}
for identifier, (row, plist, binary) in expected.items():
    old_row, old_plist, old_binary = original[identifier]
    assert binary == old_binary
    if identifier not in changed:
        assert plist == old_plist
        continue

    info = plistlib.loads(old_plist)
    props = info["IOKitPersonalities"][changed[identifier]]
    assert props["IOGLBundleName"] == "GLDriverXenosCompositor"
    props["IOGLBundleName"] = "GLDriverXenosCompositorRetry"
    assert info == plistlib.loads(plist)
    raw = compact_xml(plist)
    packed = codec["encode"](raw)
    offset, _, _, modified = struct.unpack_from(">4I", old_archive, old_row)
    next_offset = struct.unpack_from(">I", old_archive, old_row + 16)[0]
    capacity = next_offset - offset
    if len(packed) > capacity:
        raise ValueError(f"{identifier} exceeds its original archive slot")
    archive[offset:next_offset] = packed.ljust(capacity, b"\0")
    struct.pack_into(">4I", archive, old_row, offset, len(packed), len(raw), modified)
    print(f"{identifier}: {len(packed)} compressed bytes in its {capacity}-byte slot")

struct.pack_into(">I", archive, 12, zlib.adler32(archive[16:]))
assert len(archive) == len(old_archive)
decoded = codec["read_archive"](archive)
assert decoded.keys() == expected.keys()
for identifier, (offset, plist, binary) in decoded.items():
    assert offset == original[identifier][0]
    assert plistlib.loads(plist) == plistlib.loads(expected[identifier][1])
    assert binary == expected[identifier][2]
    assert struct.unpack_from(">I", archive, offset)[0] == struct.unpack_from(">I", old_archive, offset)[0]
    assert archive[offset + 16:offset + 32] == old_archive[offset + 16:offset + 32]
print(f"Verified all {len(decoded)} original offsets and executable records at {len(archive)} bytes")

firmware = verified(working / "openbios-xbox360-gpu-compositor-context.elf",
                    "7a6ed9d448391eb911a124abb4d4edd8")
old_name = b"XenosWCC.mkext\0"
archive_name = "XWCF.MKE"
firmware_name = "CMPFIX.ELF"
patched_name = (archive_name.encode() + b"\0").ljust(len(old_name), b"\0")
assert firmware.count(old_name) == 1 and archive_name.encode() + b"\0" not in firmware
patched = firmware.replace(old_name, patched_name)
assert len(patched) == len(firmware)
assert patched.replace(patched_name, old_name) == firmware

failed_conf = verified(retry / "kboot-gpu-compositor-retry.conf", "539b08d0a314f0e36bb081ea9b96d8d6")
assignments = [line for line in failed_conf.splitlines() if line and not line.startswith((b"#", b";"))]
assert assignments[0:2] == [b"default=gpucompretry", b"timeout=0"]
entries = assignments[2:]
boot_label = "gpucompbootfix"
entry = f'{boot_label}="uda0:/{firmware_name}"'.encode()
lines = [b"#KBOOTCONFIG", f"default={boot_label}".encode(), b"timeout=0", entry, *entries]
conf = b"\n".join(lines) + b"\n"
assert len(conf) < len(failed_conf)
assert len(entries) + 1 <= 64
assert len({line.split(b"=", 1)[0] for line in lines[3:]}) == len(entries) + 1
print(f"Preserved {len(entries)} earlier XeLL entries in the {len(conf)}-byte alternate configuration")
for filename, data in (
    (archive_name, archive),
    (firmware_name, patched),
    ("kboot-gpu-compositor-bootfix.conf", conf),
):
    (stage / filename).write_bytes(data)
