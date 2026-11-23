import ast
import hashlib
from pathlib import Path
import plistlib
import struct
import zlib

root = Path(__file__).resolve().parent
stage = root / "boot-stage"
base = root.parent / "gpu-windowserver/boot-stage"
codec_path = root.parent / "tx-build/repack.py"
codec_ast = ast.parse(codec_path.read_text())
codec_ast.body = [node for node in codec_ast.body if isinstance(node, ast.FunctionDef)]
codec = {"struct": struct, "zlib": zlib, "plistlib": plistlib}
exec(compile(codec_ast, str(codec_path), "exec"), codec)

archive_name = "XenosQER.mkext"
elf_name = "openbios-xbox360-gpu-windowserver-retry.elf"
config_name = "kboot-gpu-windowserver-retry.conf"
test_dir = "XenosWindowServerRetryTest"
bundle_id = "fish.goldfish64.osx360.XenonCoreGraphics"
old_archive_name = b"XenosQES.mkext\0"
source_archive = (base / "XenosQES.mkext").read_bytes()
assert hashlib.md5(source_archive).hexdigest() == "30d4ab4c38ce4b852e195a11282c5308"
old_entries = codec["read_archive"](source_archive)

_, old_plist, old_binary = old_entries[bundle_id]
info = plistlib.loads(old_plist)
for key in ("CFBundleVersion", "OSBundleCompatibleVersion"):
    assert info[key] == "0.0.5d39"
    info[key] = "0.0.5d40"
libraries = info["OSBundleLibraries"]
assert libraries.pop("com.apple.kernel.bsd") == "1.1"
assert "com.apple.kpi.bsd" not in libraries
libraries["com.apple.kpi.bsd"] = "8.11.0"
plist = plistlib.dumps(info)

# The dependency is a plist change. Reuse the G5-linked PowerPC code
# and update the version field in its existing kmod_info_32_v1 record.
assert hashlib.sha256(old_binary).hexdigest() == (
    "7e92c7b527984cd3ac0ad0d2c62dfe1a4dd784ca833ba86f59d7db4d65974df7"
)
assert struct.unpack_from(">4I", old_binary) == (0xfeedface, 18, 0, 1)
module_name = bundle_id.encode().ljust(64, b"\0")
old_version = b"0.0.5d39".ljust(64, b"\0")
new_version = b"0.0.5d40".ljust(64, b"\0")
old_fields = module_name + old_version
new_fields = module_name + new_version
assert old_binary.count(old_fields) == 1 and new_fields not in old_binary
binary = old_binary.replace(old_fields, new_fields)
assert len(binary) == len(old_binary) and binary.replace(new_fields, old_fields) == old_binary

archive = bytearray(source_archive[:32 + 32 * len(old_entries)])
for identifier, (row, _, _) in old_entries.items():
    if identifier == bundle_id:
        codec["append_payload"](archive, row, plist)
        codec["append_payload"](archive, row + 16, binary)
        continue
    for slot in (row, row + 16):
        offset, compressed, length, _ = struct.unpack_from(">4I", source_archive, slot)
        archive.extend(bytes(-len(archive) % 32))
        struct.pack_into(">I", archive, slot, len(archive))
        archive.extend(source_archive[offset:offset + (compressed or length)])
archive.extend(bytes(-len(archive) % 32))
struct.pack_into(">II", archive, 8, len(archive), zlib.adler32(archive[16:]))
new_entries = codec["read_archive"](archive)
assert new_entries.keys() == old_entries.keys()
for identifier, (_, unpacked_info, executable) in new_entries.items():
    expected = (plist, binary) if identifier == bundle_id else old_entries[identifier][1:]
    assert (unpacked_info, executable) == expected, identifier

source_elf = (base / "openbios-xbox360-gpu-windowserver.elf").read_bytes()
assert hashlib.md5(source_elf).hexdigest() == "5bec537b95a8c8da04a557dbcd74121c"
new_archive_name = archive_name.encode() + b"\0"
assert len(new_archive_name) == len(old_archive_name)
assert source_elf.count(old_archive_name) == 1 and new_archive_name not in source_elf
elf = source_elf.replace(old_archive_name, new_archive_name)
assert elf.replace(new_archive_name, old_archive_name) == source_elf

config = (base / "kboot-gpu-windowserver.conf").read_bytes()
assert hashlib.md5(config).hexdigest() == "cc2236851740a9c4c1a4b793b82ddd48"
assert config.count(b"default=gpuxenosws\n") == 1 and config.endswith(b"\n")
assert b"gpuxenoswsretry" not in config
config = config.replace(b"default=gpuxenosws\n", b"default=gpuxenoswsretry\n")
config += f'gpuxenoswsretry="uda0:/{elf_name}"\n'.encode()

original_test = base / "XenosWindowServerTest"
app = (original_test / "XenosWindowServerCheck").read_bytes()
runner = (original_test / "RunXenosWindowServer.sh").read_bytes()
notes = (root / "TestNotes.txt").read_bytes()
assert hashlib.md5(app).hexdigest() == "1daa1f580a806cd63a1c06b6b34039d8"
assert hashlib.md5(runner).hexdigest() == "b61133ec097a41c51b6390cc317858f8"
files = {
    archive_name: archive, elf_name: elf, config_name: config,
    f"{test_dir}/XenosWindowServerCheck": app,
    f"{test_dir}/RunXenosWindowServer.sh": runner,
    f"{test_dir}/TestNotes.txt": notes,
}
for name, data in files.items():
    path = stage / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    path.chmod(0o755 if name.endswith(("XenosWindowServerCheck", ".sh")) else 0o644)
    print(f"{hashlib.md5(data).hexdigest()}  {name}")

# Test the exact packed plist and executable with Tiger's offline KLD path.
contents = root / "link-preflight/retry/XenonCoreGraphics.kext/Contents"
(contents / "MacOS").mkdir(exist_ok=True, parents=True)
(contents / "Info.plist").write_bytes(new_entries[bundle_id][1])
(contents / "MacOS/XenonCoreGraphics").write_bytes(new_entries[bundle_id][2])
