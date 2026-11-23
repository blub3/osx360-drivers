import ast
import hashlib
from pathlib import Path
import plistlib
import re
import shutil
import struct
import zlib

root = Path(__file__).resolve().parent
base_dir = root.parent / "gpu-ring-buffer-retry" / "boot-stage"
stage = root / "boot-stage"
stage.mkdir(exist_ok=True)

testdir = stage / "XenosWindowTileTest"
plugin = testdir / "XenosWinTile.plugin" / "Contents"
(plugin / "MacOS").mkdir(parents=True, exist_ok=True)
factory = re.search(r'XENOS_BLIT_FACTORY "([A-F0-9-]+)"', (root / "XenosBlitABI.h").read_text())[1]
plugin_type = "ACCF0000-0000-0000-0000-000A2789904E"
(plugin / "Info.plist").write_bytes(plistlib.dumps({
    "CFBundleName": "XenosWinTile",
    "CFBundleExecutable": "XenosWinTile",
    "CFBundleIdentifier": "fish.goldfish64.osx360.XenosWinTile",
    "CFBundleInfoDictionaryVersion": "6.0",
    "CFBundlePackageType": "BNDL",
    "CFBundleShortVersionString": "1.0",
    "CFBundleVersion": "1",
    "CFPlugInDynamicRegistration": "NO",
    "CFPlugInFactories": {factory: "XenosWinTileFactory"},
    "CFPlugInTypes": {plugin_type: [factory]},
    "Personality": {"IOProviderClass": "XenonFramebufferTest"},
}))
shutil.copy2(root / "build" / "XenosWinTile", plugin / "MacOS")
for name in ("XenosWindowPluginCheck", "XenosWindowRepaintCheck"):
    shutil.copy2(root / "build" / name, testdir)

codec_path = root.parent / "tx-build" / "repack.py"
codec_module = ast.parse(codec_path.read_text())
codec_module.body = [node for node in codec_module.body if isinstance(node, ast.FunctionDef)]
codec = {"struct": struct, "zlib": zlib, "plistlib": plistlib}
exec(compile(codec_module, str(codec_path), "exec"), codec)


def verified(path, digest):
    data = path.read_bytes()
    if hashlib.md5(data).hexdigest() != digest:
        raise ValueError(f"Checksum mismatch for {path}")
    return data


base = verified(base_dir / "XWRB.MKE", "035a699f914d23541fa2709107f065dd")
original = codec["read_archive"](base)
identifier = "fish.goldfish64.osx360.XenonCoreGraphics"
row, old_plist, _ = original[identifier]
assert old_plist.count(b"0.0.5d47") == 2
plist = old_plist.replace(b"0.0.5d47", b"0.0.5d48")
binary = (root / "XenonCoreGraphics-linked").read_bytes()
assert struct.unpack_from(">4I", binary) == (0xfeedface, 18, 0, 1)
module = identifier.encode().ljust(64, b"\0")
assert binary.count(module + b"0.0.5d48".ljust(64, b"\0")) == 1
assert b"XenosRingBufferOriginalCheck\0" in binary and b"XenosTileCapture: %s ticket=" in binary

plist_offset, _, _, plist_stamp = struct.unpack_from(">4I", base, row)
binary_offset, _, _, binary_stamp = struct.unpack_from(">4I", base, row + 16)
tail_offset = struct.unpack_from(">I", base, row + 32)[0]
plist_data = codec["encode"](plist)
binary_data = codec["encode"](binary)
assert len(plist_data) <= binary_offset - plist_offset
needed = max(0, binary_offset + len(binary_data) - tail_offset)
shift = (needed + 31) & ~31
archive = bytearray(base[:tail_offset]) + bytes(shift) + base[tail_offset:]
archive[plist_offset:binary_offset] = plist_data.ljust(binary_offset - plist_offset, b"\0")
archive[binary_offset:tail_offset + shift] = binary_data.ljust(tail_offset + shift - binary_offset, b"\0")
struct.pack_into(">4I", archive, row, plist_offset, len(plist_data), len(plist), plist_stamp)
struct.pack_into(">4I", archive, row + 16, binary_offset, len(binary_data), len(binary), binary_stamp)
for record, _, _ in original.values():
    if record > row:
        for payload_row in (record, record + 16):
            old_offset = struct.unpack_from(">I", base, payload_row)[0]
            assert old_offset >= tail_offset
            struct.pack_into(">I", archive, payload_row, old_offset + shift)
framebuffer = "fish.goldfish64.osx360.XenonFramebufferTest"
fb_row, fb_old_plist, fb_binary = original[framebuffer]
assert fb_old_plist.count(b"XenosWinBatch.plugin") == 1
fb_plist = fb_old_plist.replace(b"XenosWinBatch.plugin", b"XenosWinTile.plugin")
fb_packed = codec["encode"](fb_plist)
fb_offset, _, _, fb_stamp, fb_exec = struct.unpack_from(">5I", archive, fb_row)
assert len(fb_packed) <= fb_exec - fb_offset
archive[fb_offset:fb_exec] = fb_packed.ljust(fb_exec - fb_offset, b"\0")
struct.pack_into(">4I", archive, fb_row, fb_offset, len(fb_packed), len(fb_plist), fb_stamp)
struct.pack_into(">II", archive, 8, len(archive), zlib.adler32(archive[16:]))

decoded = codec["read_archive"](archive)
assert decoded.keys() == original.keys()
for name, (record, metadata, executable) in decoded.items():
    assert record == original[name][0]
    if name == framebuffer:
        assert metadata == fb_plist and executable == fb_binary
    else:
        assert (metadata, executable) == ((plist, binary) if name == identifier else original[name][1:])
    if name != identifier:
        for field in (0, 16):
            before = struct.unpack_from(">4I", base, record + field)
            after = struct.unpack_from(">4I", archive, record + field)
            if name != framebuffer or field == 16:
                assert after == (before[0] + (shift if record > row else 0), *before[1:])
print(f"Kept eight unchanged executables and updated the framebuffer plug-in selection. Archive {len(base)} -> {len(archive)} bytes, tail shift={shift}.")

firmware = verified(base_dir / "RINGBUF.ELF", "518e5936eb46df7789de28cf5a3e01f7")
assert firmware.count(b"XWRB.MKE\0") == 1 and b"XWTC.MKE\0" not in firmware
firmware = firmware.replace(b"XWRB.MKE\0", b"XWTC.MKE\0")
config = verified(base_dir / "kboot-gpu-ring-buffer.conf", "1aedad97fcc51c11b1215eb5ebaa772a")
lines = config.splitlines()
assert lines[:4] == [b"#KBOOTCONFIG", b"default=gpuringbuf", b"timeout=0",
                     b'gpuringbuf="uda0:/RINGBUF.ELF"']
entries = [b'gpuwintile="uda0:/WINTILE.ELF"', *lines[3:]]
assert len(entries) <= 64 and len({line.split(b"=", 1)[0] for line in entries}) == len(entries)
config = b"\n".join([lines[0], b"default=gpuwintile", lines[2], *entries]) + b"\n"
print(f"Retained {len(entries) - 1} existing boot entries.")
for name, data in (("WINTILE.ELF", firmware), ("XWTC.MKE", archive),
                   ("kboot-gpu-window-tile.conf", config)):
    (stage / name).write_bytes(data)
