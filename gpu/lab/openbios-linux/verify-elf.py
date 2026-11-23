from hashlib import sha256
from pathlib import Path
from struct import unpack_from
import sys

elf = Path(sys.argv[1]).read_bytes()
assert elf[:7] == b"\x7fELF\x01\x02\x01", "Expected a 32-bit big-endian ELF"
kind, machine, version, entry, phoff, shoff, flags, ehsize, phsize, phcount = unpack_from(
    ">HHIIIIIHHH", elf, 16)
assert (kind, machine, version, ehsize, phsize) == (2, 20, 1, 52, 32)
assert phcount > 0 and phoff + phsize * phcount <= len(elf)
segments = [unpack_from(">8I", elf, phoff + i * phsize) for i in range(phcount)]
assert all(offset + filesz <= len(elf) and filesz <= memsz
           for kind, offset, va, pa, filesz, memsz, flags, align in segments if kind == 1)
assert any(kind == 1 and flags & 1 and va <= entry < va + memsz
           for kind, offset, va, pa, filesz, memsz, flags, align in segments)
assert b"HDMI audio: transmitter setup complete" in elf
assert b"xenon-hdmi-audio" in elf
print(f"PowerPC ELF, entry {entry:#x}, {len(elf):,} bytes")
print(f"SHA-256 {sha256(elf).hexdigest()}")
