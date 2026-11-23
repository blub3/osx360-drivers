import ast
import hashlib
from pathlib import Path
import plistlib
import struct
import zlib


root = Path(__file__).resolve().parent
codec_path = root.parent / "tx-build/repack.py"
codec_ast = ast.parse(codec_path.read_text())
codec_ast.body = [node for node in codec_ast.body if isinstance(node, ast.FunctionDef)]
codec = {"struct": struct, "zlib": zlib, "plistlib": plistlib}
exec(compile(codec_ast, str(codec_path), "exec"), codec)

current = root.parent / "framebuffer-test/boot-stage-v4"
original = (current / "XenosFB.mkext").read_bytes()
assert hashlib.md5(original).hexdigest() == "883676fa89a708021e4a3294c42dda83"
entries = codec["read_archive"](original)
archive = bytearray(original[:32 + 32 * len(entries)])
versions = {"XenonFramebufferTest": "0.0.5d1", "XenonSMC": "0.0.5d1"}
expected = {}

for identifier, (row, plist, binary) in entries.items():
    name = identifier.rsplit(".", 1)[1]
    if name in versions:
        build = root / "kexts" / name / "build_tiger"
        plist = (build / (name + "-Info.plist")).read_bytes()
        binary = (root / "linked" / name).read_bytes()
        metadata = plistlib.loads(plist)
        assert metadata["CFBundleIdentifier"] == identifier
        assert metadata["CFBundleVersion"] == versions[name]
        assert (versions[name].encode() + b"\0") in binary
        assert struct.unpack_from(">4I", binary) == (0xfeedface, 18, 0, 1)
        for slot, payload in ((row, plist), (row + 16, binary)):
            codec["append_payload"](archive, slot, payload)
    else:
        for slot in (row, row + 16):
            offset, compressed, length, _ = struct.unpack_from(">4I", original, slot)
            archive.extend(bytes(-len(archive) % 32))
            struct.pack_into(">I", archive, slot, len(archive))
            archive.extend(original[offset:offset + (compressed or length)])
    expected[identifier] = (plist, binary)

archive.extend(bytes(-len(archive) % 32))
struct.pack_into(">II", archive, 8, len(archive), zlib.adler32(archive[16:]))
assert {key: row[1:] for key, row in codec["read_archive"](archive).items()} == expected
assert sum(entries[key][1:] != row for key, row in expected.items()) == len(versions)

elf_name = "openbios-xbox360-native-hdmi-test.elf"
elf = (root / elf_name).read_bytes()
assert struct.unpack_from(">16sHHI", elf)[0][:6] == b"\x7fELF\x01\x02"
assert struct.unpack_from(">HH", elf, 16) == (2, 20)  # Big-endian PowerPC executable.
for marker in (b"NativeHD.mkext\0", b"xbox360-native-hdmi-profiles\0",
               b"programmed experimental 1080p60\0", b"xenon-hdmi-audio\0"):
    assert marker in elf

config = (current / "kboot-framebuffer-test.conf").read_bytes()
assert hashlib.md5(config).hexdigest() == "36487a07600bfd58dba67d0566d1008d"
config = config.replace(b"default=framebuffer\n", b"default=hdminative\n")
config += f'hdminative="uda0:/{elf_name}"\n'.encode()

stage = root / "boot-stage"
stage.mkdir(exist_ok=True)
files = {
    "NativeHD.mkext": archive,
    elf_name: elf,
    "kboot-native-hdmi-test.conf": config,
}
for name, data in files.items():
    (stage / name).write_bytes(data)
    print(f"{hashlib.md5(data).hexdigest()}  {len(data):>7}  {name}")
