#!/usr/bin/env python3
"""Record GPU profiles from a read-only copy of an OSX360BOOT volume."""

import argparse
import ast
import hashlib
from pathlib import Path
import plistlib
import re
import struct
import zlib

ROOT = Path(__file__).resolve().parents[2]
LAB = ROOT / "gpu" / "lab"
ARCHIVE = re.compile(rb"([A-Za-z0-9_.-]+\.(?:[mM][kK][eE](?:[xX][tT])?))\x00")
ENTRY = re.compile(r'(gpu[^=]+)="uda0:/(.+)"')
FIRMWARE_TAGS = {
    "gpuvbretry": "gpu-fw-vblank-reserve",
    "gpuvbqueue": "gpu-fw-vblank-queue",
}


def reader():
    path = LAB / "tx-build" / "repack.py"
    module = ast.parse(path.read_text())
    module.body = [node for node in module.body if isinstance(node, ast.FunctionDef)]
    scope = {"struct": struct, "zlib": zlib, "plistlib": plistlib}
    exec(compile(module, str(path), "exec"), scope)
    return scope["read_archive"]


def archive_name(boot, elf):
    candidates = {name.decode("ascii") for name in ARCHIVE.findall((boot / elf).read_bytes())}
    matches = [name for name in sorted(candidates) if (boot / name).is_file()]
    if len(matches) != 1:
        raise ValueError(f"{elf}: expected one on-volume archive, got {matches}")
    return matches[0]


def recipe_for(profile, elf):
    matches = set()
    for conf in LAB.glob("*/boot-stage*/kboot*.conf"):
        lines = conf.read_text().splitlines()
        if f"default={profile}" in lines and f'{profile}="uda0:/{elf}"' in lines:
            matches.add(conf.parent.parent)
    if len(matches) != 1:
        raise ValueError(f"{profile}: expected one recorded recipe, got {sorted(matches)}")
    return matches.pop()


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("boot", type=Path)
    parser.add_argument("--date", required=True, help="date that the USB was read")
    parser.add_argument("--docs", type=Path, default=ROOT / "docs")
    parser.add_argument("--staged-config", action="append", default=[], metavar="FILENAME",
                        help="include an alternate XeLL menu saved alongside kboot.conf")
    args = parser.parse_args()
    read_archive = reader()
    boot = args.boot
    config = (boot / "kboot.conf").read_text().splitlines()
    default = next(line.split("=", 1)[1] for line in config if line.startswith("default="))
    entries = [match.groups() for line in config if (match := ENTRY.fullmatch(line))]
    if len({name for name, _ in entries}) != len(entries):
        raise ValueError("duplicate GPU profile")
    active_entries = set(entries)
    staged = set()
    for filename in args.staged_config:
        if Path(filename).name != filename:
            raise ValueError("expected an on-volume config filename")
        alternate = (boot / filename).read_text().splitlines()
        selected = next(line.split("=", 1)[1] for line in alternate if line.startswith("default="))
        matches = [match.groups() for line in alternate
                   if (match := ENTRY.fullmatch(line)) and match[1] == selected]
        if len(matches) != 1:
            raise ValueError(f"{filename}: expected one GPU entry for {selected}")
        staged.update(set(matches) - active_entries)
    superseded = set()
    for saved in boot.glob("kboot.conf.before-*.bak"):
        for line in saved.read_text().splitlines():
            match = ENTRY.fullmatch(line)
            if match and match.groups() not in active_entries | staged:
                superseded.add(match.groups())
    sets = {}
    rows = []
    retired_rows = []
    staged_rows = []
    tags = []
    for profile, elf in [*entries, *sorted(staged), *sorted(superseded)]:
        archive = archive_name(boot, elf)
        earlier = recipe_for(profile, elf)
        versions = {
            key.rsplit(".", 1)[-1]: plistlib.loads(info)["CFBundleVersion"]
            for key, (_, info, _) in read_archive((boot / archive).read_bytes()).items()
        }
        core = versions.pop("XenonCoreGraphics")
        framebuffer = versions.pop("XenonFramebufferTest", "absent")
        bundle_set = tuple(sorted(versions.items()))
        if bundle_set not in sets:
            sets[bundle_set] = chr(ord("A") + len(sets))
        group = sets[bundle_set]
        retired = (profile, elf) in superseded
        tag = f"gpu-{core}-{profile}" + ("-original" if retired else "")
        tags.append(tag)
        fwtag = FIRMWARE_TAGS.get(profile, "gpu-fw-native-hdmi")
        sha = f"`{digest(boot / elf)[:12]}` / `{digest(boot / archive)[:12]}`"
        section = retired_rows if retired else staged_rows if (profile, elf) in staged else rows
        section.append(f"| `{profile}` | [{tag}](https://github.com/blub3/osx360-drivers/tree/{tag})"
                    f" | `{elf}` | `{archive}` | {core} | {framebuffer} | {group}"
                    f" | [recipe](../{earlier.relative_to(ROOT).as_posix()})"
                    f" | [{fwtag}](https://github.com/blub3/openbios/tree/{fwtag}) | {sha} |")
    header = "| XeLL entry | Driver source tag | ELF | mkext | CoreGraphics | FramebufferTest | Set | Build files | OpenBIOS source tag | SHA-256 ELF / mkext |"
    divider = "| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |"
    pages = [
        "# OSX360BOOT GPU profiles",
        "",
        f"Read from the mounted test USB on {args.date}. The current default is **`{default}`**.",
        f"The table covers all {len(entries)} `gpu*` entries. The two graphics versions and the",
        "dependency set together identify every kext in each archive.",
        "",
        "The last column shows the first 12 characters of the ELF and mkext SHA-256",
        "checksums. [The full manifest](gpu-boot-checksums.txt) also includes the active",
        "configuration, rollback configs, saved test images, and the HDMI and 720p backups.",
        "The driver tags preserve the source and profile recipes. Firmware tags identify",
        "the ELF source, with archive-name-only images derived from the native HDMI tag.",
        "See [rebuilding a profile](../gpu/README.md) for the toolchain and reference inputs.",
        "",
        header, divider, *rows, "",
    ]
    if staged_rows:
        pages.extend([
            "## Staged entries in alternate menus", "",
            "These candidates have separate XeLL menus. The active menu is unchanged.", "",
            header, divider, *staged_rows, "",
        ])
    pages.extend([
        "## Superseded entries retained in rollback menus", "",
        header, divider, *retired_rows, "",
        "## Other kexts in the archives", "",
        "| Set | Additional kexts |", "| --- | --- |",
    ])
    for versions, group in sets.items():
        kexts = ", ".join(f"`{name} {version}`" for name, version in versions)
        pages.append(f"| {group} | {kexts} |")
    args.docs.mkdir(parents=True, exist_ok=True)
    (args.docs / "gpu-boot-profiles.md").write_text("\n".join(pages) + "\n")
    include = re.compile(r"\.(?:elf|mkext|mke|bak)$", re.IGNORECASE)
    files = (p for p in boot.iterdir() if p.is_file() and
             (p.name == "kboot.conf" or p.name in args.staged_config or include.search(p.name)))
    manifest = "".join(f"{digest(path)}  {path.name}\n" for path in sorted(files))
    (args.docs / "gpu-boot-checksums.txt").write_text(manifest)
    print("\n".join(tags))


if __name__ == "__main__":
    main()
