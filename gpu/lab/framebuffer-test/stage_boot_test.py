import argparse
import ast
import hashlib
from pathlib import Path
import plistlib
import struct
import zlib


root = Path(__file__).resolve().parent
outputs = root.parent.parent / "outputs"
parser = argparse.ArgumentParser()
parser.add_argument("--bundle", type=Path, default=root / "XenonFramebufferTest.kext")
parser.add_argument("--stage", type=Path, default=root / "boot-stage")
args = parser.parse_args()
stage = args.stage
stage.mkdir(parents=True, exist_ok=True)

# Reuse the codec already verified against the working Ethernet archive.
codec_path = root.parent / "tx-build" / "repack.py"
codec_ast = ast.parse(codec_path.read_text())
codec_ast.body = [node for node in codec_ast.body if isinstance(node, ast.FunctionDef)]
codec = {"struct": struct, "zlib": zlib, "plistlib": plistlib}
exec(compile(codec_ast, str(codec_path), "exec"), codec)
read_archive = codec["read_archive"]
append_payload = codec["append_payload"]

original = (outputs / "Xbox360.mkext").read_bytes()
assert hashlib.md5(original).hexdigest() == "c9fe3cf65d40779be40b68065e5b14e9"
original_entries = read_archive(original)
test = args.bundle / "Contents"
graphics = root / "tiger-graphics"
added = [
    (graphics / "Info.plist", graphics / "IOGraphicsFamily"),
    (test / "Info.plist", test / "MacOS" / "XenonFramebufferTest"),
]
header_size = 32
record_size = 32
original_table_end = header_size + record_size * len(original_entries)
shift = record_size * len(added)
archive = bytearray(original[:original_table_end] + bytes(shift) + original[original_table_end:])

for position, _, _ in original_entries.values():
    for slot in (position, position + 16):
        offset, = struct.unpack_from(">I", original, slot)
        assert offset >= original_table_end
        struct.pack_into(">I", archive, slot, offset + shift)

expected_payloads = {identifier: entry[1:] for identifier, entry in original_entries.items()}
for index, (info_path, binary_path) in enumerate(added):
    info, binary = info_path.read_bytes(), binary_path.read_bytes()
    identifier = plistlib.loads(info)["CFBundleIdentifier"]
    assert identifier not in expected_payloads
    assert struct.unpack_from(">III", binary) == (0xfeedface, 18, 0)
    expected_payloads[identifier] = (info, binary)
    record = original_table_end + index * record_size
    append_payload(archive, record, info)
    append_payload(archive, record + 16, binary)

archive.extend(bytes(-len(archive) % 32))
struct.pack_into(">I", archive, 20, len(expected_payloads))
struct.pack_into(">II", archive, 8, len(archive), zlib.adler32(archive[16:]))
assert {identifier: entry[1:] for identifier, entry in read_archive(archive).items()} == expected_payloads
(stage / "XenosFB.mkext").write_bytes(archive)

working_elf = (outputs / "openbios-xbox360-1080p-audio-test.elf").read_bytes()
assert hashlib.md5(working_elf).hexdigest() == "a95a04400a7483e3cffe54b76b8197a7"
original_name = b"Xbox360.mkext\0"
test_name = b"XenosFB.mkext\0"
assert len(original_name) == len(test_name) and working_elf.count(original_name) == 1
assert test_name not in working_elf
patched_elf = working_elf.replace(original_name, test_name)
assert len(patched_elf) == len(working_elf)
assert patched_elf.replace(test_name, original_name) == working_elf
elf_name = "openbios-xbox360-framebuffer-test.elf"
(stage / elf_name).write_bytes(patched_elf)

config = (root / "kboot.working.conf").read_bytes()
assert hashlib.md5(config).hexdigest() == "937e000a82e6cabc475555f871a85c13"
assert config.count(b"default=openbios") == 1
assert b'timeout=0' in config
newline = b"\r\n" if b"\r\n" in config else b"\n"
config = config.replace(b"default=openbios", b"default=framebuffer")
config = config.rstrip(b"\r\n") + newline + f'framebuffer="uda0:/{elf_name}"'.encode() + newline
(stage / "kboot-framebuffer-test.conf").write_bytes(config)

for path in sorted(stage.iterdir()):
    data = path.read_bytes()
    print(f"{hashlib.md5(data).hexdigest()}  {len(data):>7}  {path.name}")
