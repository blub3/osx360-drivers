import ast
import hashlib
from pathlib import Path
import plistlib
import re
import shutil
import struct
import zlib

root = Path(__file__).resolve().parent
stage = root / "boot-stage"
working = root.parent / "gpu-window-repaint-retry" / "boot-stage"
testdir = stage / "XenosWindowMapTest"
plugin = testdir / "XenosWinMap.plugin" / "Contents"
(plugin / "MacOS").mkdir(parents=True, exist_ok=True)
factory = "10548CB1-3E2A-43EC-9CEE-2FA09017E6B0"
plugin_type = "ACCF0000-0000-0000-0000-000A2789904E"
(plugin / "Info.plist").write_bytes(plistlib.dumps({
    "CFBundleName": "XenosWinMap",
    "CFBundleExecutable": "XenosWinMap",
    "CFBundleIdentifier": "fish.goldfish64.osx360.XenosWinMap",
    "CFBundleInfoDictionaryVersion": "6.0",
    "CFBundlePackageType": "BNDL",
    "CFBundleShortVersionString": "1.0",
    "CFBundleVersion": "1",
    "CFPlugInDynamicRegistration": "NO",
    "CFPlugInFactories": {factory: "XenosWinMapFactory"},
    "CFPlugInTypes": {plugin_type: [factory]},
    "Personality": {"IOProviderClass": "XenonFramebufferTest"},
}))
shutil.copy2(root / "build" / "XenosWinMap", plugin / "MacOS")
shutil.copy2(root / "build" / "XenosWindowPluginCheck", testdir)
shutil.copy2(root / "build" / "XenosWindowRepaintCheck", testdir)

codec_file = root.parent / "tx-build" / "repack.py"
code = ast.parse(codec_file.read_text())
code.body = [node for node in code.body if isinstance(node, ast.FunctionDef)]
codec = {"struct": struct, "zlib": zlib, "plistlib": plistlib}
exec(compile(code, str(codec_file), "exec"), codec)


def verified(path, digest):
    data = path.read_bytes()
    if hashlib.md5(data).hexdigest() != digest:
        raise ValueError(f"Checksum mismatch for {path}")
    return data


base = verified(working / "XWRR.MKE", "8de0a6ef81b355802c4a6d6a313ec593")
original = codec["read_archive"](base)
identifier = "fish.goldfish64.osx360.XenonFramebufferTest"
row, old_plist, old_binary = original[identifier]
metadata = plistlib.loads(old_plist)
props = metadata["IOKitPersonalities"]["XenonFramebufferTest"]
assert props["IOGLBundleName"] == "GLDriverXenosCompositorRetry"
key = "ACCF0000-0000-0000-0000-000a2789904e"
assert props["IOCFPlugInTypes"] == {key: "XenosWinFix.plugin"}
assert "IOAccelRevision" not in props
props["IOCFPlugInTypes"][key] = "XenosWinMap.plugin"
for name in ("CFBundleVersion", "OSBundleCompatibleVersion"):
    if name in metadata:
        assert metadata[name] == "0.0.5d2"
        metadata[name] = "0.0.5d3"
raw = plistlib.dumps(metadata, sort_keys=False)
raw = re.sub(rb">[\t \n\r]+<", b"><", raw)
raw = re.sub(rb"<!DOCTYPE [^>]+>", b"", raw)
packed = codec["encode"](raw)
offset, _, _, modified, next_offset = struct.unpack_from(">5I", base, row)
capacity = next_offset - offset
assert plistlib.loads(raw) == metadata
if len(packed) > capacity:
    raise ValueError(f"{len(packed)} compressed bytes exceed {capacity}-byte plist slot")
print(f"Framebuffer plist uses {len(packed)} of {capacity} bytes.")

binary = (root / "XenonFramebufferTest-linked").read_bytes()
assert struct.unpack_from(">4I", binary) == (0xfeedface, 18, 0, 1)
module = identifier.encode().ljust(64, b"\0")
assert binary.count(module + b"0.0.5d3".ljust(64, b"\0")) == 1
assert b"XenosWindowApertureClient\0" in binary
code = codec["encode"](binary)
archive = bytearray(base)
archive[offset:next_offset] = packed.ljust(capacity, b"\0")
struct.pack_into(">4I", archive, row, offset, len(packed), len(raw), modified)
binary_offset, old_len, _, binary_modified = struct.unpack_from(">4I", base, row+16)
assert row == 32 + (len(original)-1)*32
assert binary_offset == next_offset and len(base) - (binary_offset + old_len) < 32
archive[binary_offset:] = code
archive.extend(bytes(-len(archive) % 32))
struct.pack_into(">4I", archive, row+16, binary_offset, len(code), len(binary), binary_modified)
struct.pack_into(">II", archive, 8, len(archive), zlib.adler32(archive[16:]))
decoded = codec["read_archive"](archive)
assert decoded.keys() == original.keys()
for entry, (record, payload, executable) in decoded.items():
    old_record, old_payload, old_executable = original[entry]
    assert record == old_record
    for offset_in_row in (0, 16):
        start = record + offset_in_row
        assert archive[start:start+4] == base[start:start+4]
    if entry == identifier:
        assert plistlib.loads(payload) == metadata and executable == binary
    else:
        assert payload == old_payload and executable == old_executable
        assert archive[record:record+32] == base[record:record+32]
print(f"Kept every payload offset and eight original executables. Archive length {len(base)} → {len(archive)}.")

firmware = verified(working / "WINRETRY.ELF", "a113aae33d1bdd6199be768fdc7c8a5b")
assert firmware.count(b"XWRR.MKE\0") == 1 and b"XWFM.MKE\0" not in firmware
updated = firmware.replace(b"XWRR.MKE\0", b"XWFM.MKE\0")
assert updated.replace(b"XWFM.MKE\0", b"XWRR.MKE\0") == firmware
config = verified(working / "kboot-gpu-window-retry.conf", "a9c544c6bf6ab532695cc776f7804c13")
lines = config.splitlines()
assert lines[:4] == [b"#KBOOTCONFIG", b"default=gpuwinretry", b"timeout=0",
                     b'gpuwinretry="uda0:/WINRETRY.ELF"']
entries = [b'gpuwinmap="uda0:/WINMAP.ELF"', *lines[3:]]
assert len(entries) <= 64 and len({entry.split(b"=", 1)[0] for entry in entries}) == len(entries)
config = b"\n".join([lines[0], b"default=gpuwinmap", lines[2], *entries]) + b"\n"
print(f"Retained {len(entries)-1} earlier boot entries.")
for name, payload in (("XWFM.MKE", archive), ("WINMAP.ELF", updated),
                      ("kboot-gpu-window-map.conf", config)):
    (stage / name).write_bytes(payload)
