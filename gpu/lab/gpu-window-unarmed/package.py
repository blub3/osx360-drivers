"""Stage a separate plug-in and reuse the verified gpuwintile kext executables."""

import argparse
import ast
import hashlib
from pathlib import Path
import plistlib
import shutil
import struct
import zlib

ROOT = Path(__file__).resolve().parent
SOURCE_HASHES = {
    "XWTC.MKE": "c9ad00948853d2afd4621457f90cf0adb015b9fa199cd678c6eff8d79191f73a",
    "WINTILE.ELF": "e09ddcefeb12a69378193ef0d4c53644e1b2d03c98580858c5471bb0887252b8",
}


def mkext_codec():
    path = ROOT.parent / "tx-build" / "repack.py"
    module = ast.parse(path.read_text())
    module.body = [node for node in module.body if isinstance(node, ast.FunctionDef)]
    scope = {"struct": struct, "zlib": zlib, "plistlib": plistlib}
    exec(compile(module, str(path), "exec"), scope)
    return scope["read_archive"], scope["encode"]


def verified(path):
    data = path.read_bytes()
    if hashlib.sha256(data).hexdigest() != SOURCE_HASHES[path.name]:
        raise ValueError(f"Expected the tested gpuwintile input at {path}")
    return data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("parent", type=Path, help="copy of the working OSX360BOOT volume")
    parser.add_argument("build", type=Path, help="output from build.sh")
    parser.add_argument("stage", type=Path, help="new staging directory")
    args = parser.parse_args()

    read_archive, encode = mkext_codec()
    archive = bytearray(verified(args.parent / "XWTC.MKE"))
    original = read_archive(archive)
    framebuffer = "fish.goldfish64.osx360.XenonFramebufferTest"
    record, old_plist, _ = original[framebuffer]
    old_selection = b"XenosWinTile.plugin"
    if old_plist.count(old_selection) != 1:
        raise ValueError("Expected one XenosWinTile registration")
    new_plist = old_plist.replace(old_selection, b"XenosWinAuto.plugin")
    plist_offset, _, _, stamp, executable_offset = struct.unpack_from(">5I", archive, record)
    packed = encode(new_plist)
    if len(packed) > executable_offset - plist_offset:
        raise ValueError("The updated framebuffer plist does not fit")
    archive[plist_offset:executable_offset] = packed.ljust(executable_offset - plist_offset, b"\0")
    struct.pack_into(">4I", archive, record, plist_offset, len(packed), len(new_plist), stamp)
    struct.pack_into(">II", archive, 8, len(archive), zlib.adler32(archive[16:]))
    expected = dict(original)
    expected[framebuffer] = (record, new_plist, original[framebuffer][2])
    if read_archive(archive) != expected:
        raise ValueError("Unexpected mkext payload change")

    firmware = verified(args.parent / "WINTILE.ELF")
    if firmware.count(b"XWTC.MKE\0") != 1 or b"XWAU.MKE\0" in firmware:
        raise ValueError("Expected one source firmware archive name")
    firmware = firmware.replace(b"XWTC.MKE\0", b"XWAU.MKE\0")

    config = (args.parent / "kboot.conf").read_bytes()
    lines = config.splitlines(keepends=True)
    if lines[:4] != [b"#KBOOTCONFIG\n", b"default=gpuwintile\n", b"timeout=0\n",
                     b'gpuwintile="uda0:/WINTILE.ELF"\n']:
        raise ValueError("Expected gpuwintile as the active profile")
    candidate_entry = b'gpuwinauto="uda0:/WINAUTO.ELF"\n'
    keys = [line.split(b"=", 1)[0] for line in lines[3:] if b"=" in line]
    if b"gpuwinauto" in keys or len(keys) >= 64 or len(keys) != len(set(keys)):
        raise ValueError("Cannot add a unique XeLL menu entry")
    candidate_config = b"".join([lines[0], b"default=gpuwinauto\n", lines[2], candidate_entry, *lines[3:]])
    recorded_config = ROOT / "boot-stage" / "kboot-gpu-window-unarmed.conf"
    if candidate_config != recorded_config.read_bytes():
        raise ValueError("The active menu differs from the recorded parent menu")

    out = args.stage
    out.mkdir()
    (out / "XWAU.MKE").write_bytes(archive)
    (out / "WINAUTO.ELF").write_bytes(firmware)
    (out / recorded_config.name).write_bytes(candidate_config)
    (out / "kboot.conf.before-gpuwinauto-stage.bak").write_bytes(config)
    testdir = out / "XenosWindowUnarmedTest"
    shutil.copytree(ROOT / "boot-stage" / testdir.name, testdir)
    plugin = testdir / "XenosWinAuto.plugin" / "Contents"
    (plugin / "MacOS").mkdir()
    for name in ("XenosWindowPluginCheck", "XenosWindowRepaintCheck"):
        shutil.copy2(args.build / name, testdir)
    shutil.copy2(args.build / "XenosWinAuto", plugin / "MacOS")
    print(f"Verified {len(original)} byte-identical kext executables and built the alternate XeLL menu.")


if __name__ == "__main__":
    main()
