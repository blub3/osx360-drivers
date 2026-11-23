"""Stage a separate plug-in and reuse the verified gpuwintile kext executables."""

import argparse
import ast
import hashlib
from pathlib import Path
import plistlib
import re
import shutil
import struct
import zlib

ROOT = Path(__file__).resolve().parent
SOURCE_HASHES = {
    "XWTC.MKE": "c9ad00948853d2afd4621457f90cf0adb015b9fa199cd678c6eff8d79191f73a",
    "WINTILE.ELF": "e09ddcefeb12a69378193ef0d4c53644e1b2d03c98580858c5471bb0887252b8",
    "XWAU.MKE": "c173f317c13180d64ddd719f355128c32ecfe2fc47d814e32ab14e062bca00cd",
    "WINAUTO.ELF": "907ad6641d5ea52ff6b46435f6ddfad63327fac4d52323a8381e6d3dc81728d7",
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
        raise ValueError(f"Expected the verified parent input at {path}")
    return data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("parent", type=Path, help="copy of the working OSX360BOOT volume")
    parser.add_argument("build", type=Path, help="output from build.sh")
    parser.add_argument("stage", type=Path, help="new staging directory")
    parser.add_argument("--overlap", action="store_true", help="package the larger overlapping-copy test")
    args = parser.parse_args()

    if args.overlap:
        parent_profile, parent_elf, parent_archive, parent_plugin = "gpuwinauto", "WINAUTO.ELF", "XWAU.MKE", "XenosWinAuto"
        profile, elf, archive_name, plugin_name = "gpuwinoverlap", "WINOVR.ELF", "XWOV.MKE", "XenosWinOverlap"
        template, test_dir = "kboot-gpu-window-unarmed-overlap.conf", "XenosWindowOverlapTest"
    else:
        parent_profile, parent_elf, parent_archive, parent_plugin = "gpuwintile", "WINTILE.ELF", "XWTC.MKE", "XenosWinTile"
        profile, elf, archive_name, plugin_name = "gpuwinauto", "WINAUTO.ELF", "XWAU.MKE", "XenosWinAuto"
        template, test_dir = "kboot-gpu-window-unarmed.conf", "XenosWindowUnarmedTest"

    read_archive, encode = mkext_codec()
    archive = bytearray(verified(args.parent / parent_archive))
    original = read_archive(archive)
    framebuffer = "fish.goldfish64.osx360.XenonFramebufferTest"
    record, old_plist, _ = original[framebuffer]
    old_selection = f"{parent_plugin}.plugin".encode()
    if old_plist.count(old_selection) != 1:
        raise ValueError(f"Expected one {parent_plugin} registration")
    new_plist = old_plist.replace(old_selection, f"{plugin_name}.plugin".encode())
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

    firmware = verified(args.parent / parent_elf)
    old_name, new_name = f"{parent_archive}\0".encode(), f"{archive_name}\0".encode()
    if firmware.count(old_name) != 1 or new_name in firmware:
        raise ValueError("Expected one source firmware archive name")
    firmware = firmware.replace(old_name, new_name)

    config = (args.parent / "kboot.conf").read_bytes()
    lines = config.splitlines(keepends=True)
    if lines[:4] != [b"#KBOOTCONFIG\n", f"default={parent_profile}\n".encode(), b"timeout=0\n",
                     f'{parent_profile}="uda0:/{parent_elf}"\n'.encode()]:
        raise ValueError(f"Expected {parent_profile} as the active profile")
    candidate_entry = f'{profile}="uda0:/{elf}"\n'.encode()
    keys = [line.split(b"=", 1)[0] for line in lines[3:] if b"=" in line]
    if profile.encode() in keys or len(keys) >= 64 or len(keys) != len(set(keys)):
        raise ValueError("Cannot add a unique XeLL menu entry")
    candidate_config = b"".join([lines[0], f"default={profile}\n".encode(), lines[2], candidate_entry, *lines[3:]])
    recorded_config = ROOT / "boot-stage" / template
    if candidate_config != recorded_config.read_bytes():
        raise ValueError("The active menu differs from the recorded parent menu")

    out = args.stage
    out.mkdir()
    (out / archive_name).write_bytes(archive)
    (out / elf).write_bytes(firmware)
    (out / recorded_config.name).write_bytes(candidate_config)
    (out / f"kboot.conf.before-{profile}-stage.bak").write_bytes(config)
    testdir = out / test_dir
    plugin = testdir / f"{plugin_name}.plugin" / "Contents"
    (plugin / "MacOS").mkdir(parents=True)
    reference = ROOT / "boot-stage" / "XenosWindowUnarmedTest"
    runner = "RunXenosWindowUnarmed.sh"
    shutil.copy2(reference / runner, testdir)
    info = (reference / "XenosWinAuto.plugin/Contents/Info.plist").read_text()
    if args.overlap:
        header = ROOT.parent / "gpu-window-tile-capture/XenosBlitABI.h"
        factory = re.search(r'XENOS_UNARMED_OVERLAP\)\s+#define XENOS_BLIT_FACTORY "([A-F0-9-]+)"',
                            header.read_text())[1]
        info = info.replace("XenosWinAuto", plugin_name).replace("1219E9F2-0EA1-42B0-9CE7-BEC7864F4B2B", factory)
    plistlib.loads(info.encode())
    (plugin / "Info.plist").write_text(info)
    for name in ("XenosWindowPluginCheck", "XenosWindowRepaintCheck"):
        shutil.copy2(args.build / name, testdir)
    shutil.copy2(args.build / plugin_name, plugin / "MacOS")
    print(f"Verified {len(original)} byte-identical kext executables and built the alternate XeLL menu.")


if __name__ == "__main__":
    main()
