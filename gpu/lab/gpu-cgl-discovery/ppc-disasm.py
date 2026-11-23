import os
from pathlib import Path
import re
import struct
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).with_name("disasm")))
from capstone import Cs, CS_ARCH_PPC, CS_MODE_BIG_ENDIAN, CS_MODE_32
path, *targets = sys.argv[1:]
data = Path(path).read_bytes()
assert data[:4] == b'\xfe\xed\xfa\xce'
ncmds = struct.unpack_from(">I", data, 16)[0]
off = 28
sections = {}
for _ in range(ncmds):
    cmd, length = struct.unpack_from(">II", data, off)
    if cmd == 1:
        nsects = struct.unpack_from(">I", data, off+48)[0]
        for index in range(nsects):
            sec = off + 56 + index * 68
            name, segment, address, size, disk = struct.unpack_from(">16s16sIII", data, sec)
            sections[name.rstrip(b"\0").decode()] = address, size, disk
    off += length
nm = subprocess.check_output(["/Library/Developer/CommandLineTools/usr/bin/nm-classic", "-nm", path], text=True)
byaddr={}
byname={}
for line in nm.splitlines():
    m = re.match(r"^([0-9a-f]{8}) \([^)]*\) .*?(_\S+)$",line)
    if m:
        address = int(m[1],16)
        byaddr[address] = m[2]
        byname[m[2]] = address
md = Cs(CS_ARCH_PPC, CS_MODE_BIG_ENDIAN | CS_MODE_32)
md.skipdata=True
text_address, text_size, text_disk = sections['__text']
for target in targets:
    s,b,n = (target + ":").partition(":")
    address = byname[s] if s in byname else int(s,16)
    end = address + int(n.rstrip(":") or "0",0) if n.rstrip(":") else min((x for x in byaddr if address < x <= text_address+text_size), default=text_address+text_size)
    print(f"{s} [{address:08x}..{end:08x}]:")
    start = text_disk + address - text_address
    for i in md.disasm(data[start:start+end-address], address):
        o = i.op_str
        if i.mnemonic.startswith('b'):
            dest = re.search(r'0x([a-f0-9]+)$',o)
            if dest:
                sym = byaddr.get(int(dest[1],16))
                if sym:
                    o += f" <{sym}>"
        print(f"{i.address:08x} {i.mnemonic:9} {o}")
