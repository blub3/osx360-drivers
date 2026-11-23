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
working = root.parent / "gpu-compositor-bootfix" / "boot-stage"
testdir = stage / "XenosWindowRetryTest"
plugin = testdir / "XenosWinFix.plugin" / "Contents"
(plugin / "MacOS").mkdir(parents=True, exist_ok=True)
factory = "36DC8806-C910-457B-ABFC-640F5A7251CC"
plugin_type = "ACCF0000-0000-0000-0000-000A2789904E"
(plugin / "Info.plist").write_bytes(plistlib.dumps({
    "CFBundleName": "XenosWinFix",
    "CFBundleExecutable": "XenosWinFix",
    "CFBundleIdentifier": "fish.goldfish64.osx360.XenosWinFix",
    "CFBundleInfoDictionaryVersion": "6.0",
    "CFBundlePackageType": "BNDL",
    "CFBundleShortVersionString": "1.0",
    "CFBundleVersion": "1",
    "CFPlugInDynamicRegistration": "NO",
    "CFPlugInFactories": {factory: "XenosWinFixFactory"},
    "CFPlugInTypes": {plugin_type: [factory]},
    "Personality": {"IOProviderClass": "XenonFramebufferTest"},
}))
shutil.copy2(root / "build" / "XenosWinFix", plugin / "MacOS")
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


base = verified(working / "XWCF.MKE", "c603562c9452658c09f5e25db66ff936")
original = codec["read_archive"](base)
identifier = "fish.goldfish64.osx360.XenonFramebufferTest"
row, old_plist, _ = original[identifier]
metadata = plistlib.loads(old_plist)
props = metadata["IOKitPersonalities"]["XenonFramebufferTest"]
assert props["IOGLBundleName"] == "GLDriverXenosCompositorRetry"
assert "IOCFPlugInTypes" not in props and "IOAccelRevision" not in props
props["IOCFPlugInTypes"] = {"ACCF0000-0000-0000-0000-000a2789904e": "XenosWinFix.plugin"}
raw = plistlib.dumps(metadata, sort_keys=False)
raw = re.sub(rb">[\t \n\r]+<", b"><", raw)
packed = codec["encode"](raw)
offset, _, _, modified, next_offset = struct.unpack_from(">5I", base, row)
capacity = next_offset - offset
if len(packed) > capacity:
    # The plist's type and version are declared by its root element.
    raw = re.sub(rb"<!DOCTYPE [^>]+>", b"", raw)
    packed = codec["encode"](raw)
assert plistlib.loads(raw) == metadata
if len(packed) > capacity:
    raise ValueError(f"{len(packed)} compressed bytes exceed {capacity}-byte slot")
archive = bytearray(base)
archive[offset:next_offset] = packed.ljust(capacity, b"\0")
struct.pack_into(">4I", archive, row, offset, len(packed), len(raw), modified)
struct.pack_into(">I", archive, 12, zlib.adler32(archive[16:]))
assert len(archive) == len(base)
decoded = codec["read_archive"](archive)
assert decoded.keys() == original.keys()
for key, (record, plist, binary) in decoded.items():
    old_record, previous_plist, previous_binary = original[key]
    assert record == old_record and binary == previous_binary
    assert archive[record:record+4] == base[record:record+4]
    assert archive[record+16:record+32] == base[record+16:record+32]
    if key == identifier:
        assert plistlib.loads(plist) == metadata
    else:
        assert plist == previous_plist
print(f"Framebuffer plist uses {len(packed)} of {capacity} bytes. All nine executable records and offsets match.")

firmware = verified(working / "CMPFIX.ELF", "3d5be96c3df8c4c3cdfc514c7c637ba1")
assert firmware.count(b"XWCF.MKE\0") == 1 and b"XWRR.MKE\0" not in firmware
updated = firmware.replace(b"XWCF.MKE\0", b"XWRR.MKE\0")
assert updated.replace(b"XWRR.MKE\0", b"XWCF.MKE\0") == firmware
config = verified(working / "kboot-gpu-compositor-bootfix.conf", "c76086865a738b203ebe3aab6f628dd5")
lines = config.splitlines()
assert lines[:4] == [b"#KBOOTCONFIG", b"default=gpucompbootfix", b"timeout=0",
                     b'gpucompbootfix="uda0:/CMPFIX.ELF"']
previous = verified(root.parent / "gpu-window-repaint" / "boot-stage" /
                    "kboot-gpu-window-repaint.conf", "348ca1649e90bea8a13541525f9ac0d6")
previous_lines = previous.splitlines()
assert previous_lines[3:7] == [b'gpuwinrepaint="uda0:/WINPAINT.ELF"',
                               b'gpufbblitbeam="uda0:/BLITBEAM.ELF"',
                               b'gpufbblit="uda0:/BLITSURF.ELF"', lines[3]]
assert previous_lines[6:] == lines[3:]
entries = [b'gpuwinretry="uda0:/WINRETRY.ELF"', *previous_lines[3:]]
assert len(entries) <= 64 and len({entry.split(b"=", 1)[0] for entry in entries}) == len(entries)
config = b"\n".join([lines[0], b"default=gpuwinretry", lines[2], *entries]) + b"\n"
print(f"Retained {len(entries)-1} earlier boot entries.")
for name, payload in (("XWRR.MKE", archive), ("WINRETRY.ELF", updated),
                      ("kboot-gpu-window-retry.conf", config)):
    (stage / name).write_bytes(payload)
