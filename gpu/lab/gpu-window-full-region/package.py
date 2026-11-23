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
working = root.parent / "gpu-window-framebuffer-map" / "boot-stage"
testdir = stage / "XenosWindowFullTest"
plugin = testdir / "XenosWinAll.plugin" / "Contents"
(plugin / "MacOS").mkdir(parents=True, exist_ok=True)
factory = "FDD845D0-C55D-4A10-BA30-FDFEB714685B"
plugin_type = "ACCF0000-0000-0000-0000-000A2789904E"
(plugin / "Info.plist").write_bytes(plistlib.dumps({
    "CFBundleName": "XenosWinAll",
    "CFBundleExecutable": "XenosWinAll",
    "CFBundleIdentifier": "fish.goldfish64.osx360.XenosWinAll",
    "CFBundleInfoDictionaryVersion": "6.0",
    "CFBundlePackageType": "BNDL",
    "CFBundleShortVersionString": "1.0",
    "CFBundleVersion": "1",
    "CFPlugInDynamicRegistration": "NO",
    "CFPlugInFactories": {factory: "XenosWinAllFactory"},
    "CFPlugInTypes": {plugin_type: [factory]},
    "Personality": {"IOProviderClass": "XenonFramebufferTest"},
}))
shutil.copy2(root / "build" / "XenosWinAll", plugin / "MacOS")
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


base = verified(working / "XWFM.MKE", "1b39f90c3118bd04aca9e378a10806e5")
original = codec["read_archive"](base)
identifier = "fish.goldfish64.osx360.XenonFramebufferTest"
row, old_plist, _ = original[identifier]
metadata = plistlib.loads(old_plist)
props = metadata["IOKitPersonalities"]["XenonFramebufferTest"]
assert props["IOGLBundleName"] == "GLDriverXenosCompositorRetry"
key = "ACCF0000-0000-0000-0000-000a2789904e"
assert props["IOCFPlugInTypes"] == {key: "XenosWinMap.plugin"}
assert "IOAccelRevision" not in props
props["IOCFPlugInTypes"][key] = "XenosWinAll.plugin"
assert metadata["CFBundleVersion"] == "0.0.5d3"
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

archive = bytearray(base)
archive[offset:next_offset] = packed.ljust(capacity, b"\0")
struct.pack_into(">4I", archive, row, offset, len(packed), len(raw), modified)
struct.pack_into(">I", archive, 12, zlib.adler32(archive[16:]))
decoded = codec["read_archive"](archive)
assert decoded.keys() == original.keys()
for entry, (record, payload, executable) in decoded.items():
    old_record, old_payload, old_executable = original[entry]
    assert record == old_record
    assert archive[record+16:record+32] == base[record+16:record+32]
    assert archive[record:record+4] == base[record:record+4]
    assert executable == old_executable
    if entry == identifier:
        assert plistlib.loads(payload) == metadata
    else:
        assert payload == old_payload
        assert archive[record:record+32] == base[record:record+32]
assert len(archive) == len(base)
print(f"Kept all {len(original)} executables, payload offsets and archive length ({len(base)} bytes).")

firmware = verified(working / "WINMAP.ELF", "9b046f3fbcc557cb816fde87b91f1f88")
assert firmware.count(b"XWFM.MKE\0") == 1 and b"XWFR.MKE\0" not in firmware
updated = firmware.replace(b"XWFM.MKE\0", b"XWFR.MKE\0")
assert updated.replace(b"XWFR.MKE\0", b"XWFM.MKE\0") == firmware
config = verified(working / "kboot-gpu-window-map.conf", "78be67e139d89a01fc952d5960c7a149")
lines = config.splitlines()
assert lines[:4] == [b"#KBOOTCONFIG", b"default=gpuwinmap", b"timeout=0",
                     b'gpuwinmap="uda0:/WINMAP.ELF"']
entries = [b'gpuwinfull="uda0:/WINFULL.ELF"', *lines[3:]]
assert len(entries) <= 64 and len({entry.split(b"=", 1)[0] for entry in entries}) == len(entries)
config = b"\n".join([lines[0], b"default=gpuwinfull", lines[2], *entries]) + b"\n"
print(f"Retained {len(entries)-1} earlier boot entries.")
for name, payload in (("XWFR.MKE", archive), ("WINFULL.ELF", updated),
                      ("kboot-gpu-window-full.conf", config)):
    (stage / name).write_bytes(payload)
