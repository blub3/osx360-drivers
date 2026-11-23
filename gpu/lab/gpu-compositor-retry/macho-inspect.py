from pathlib import Path
import struct
import sys

path=Path(sys.argv[1])
data=path.read_bytes()
assert data[:4]==bytes.fromhex("feedface")
segments=[]
sections={}
position=28
for index in range(struct.unpack_from(">I",data,16)[0]):
    command,size=struct.unpack_from(">II",data,position)
    if command==1:
        vm,size_memory,off,size_file=struct.unpack_from(">4I",data,position+24)
        segments.append((vm, size_file, off))
        for i in range(struct.unpack_from(">I",data,position+48)[0]):
            start=position+56+i*68
            name,segment,addr,length,disk=struct.unpack_from(">16s16sIII",data,start)
            sections[name.rstrip(b"\0").decode()]=(addr,length,disk)
    position+=size
textaddr,size,disk=sections["__text"]
for target in sys.argv[2:]:
    if target.startswith('@'):
        dest=int(target[1:],16)
        for offset in range(0,size,4):
            word=struct.unpack_from(">I",data,disk+offset)[0]
            if word >> 26 != 18:
                continue
            displacement=word & 0x03fffffc
            if displacement & 0x02000000:
                displacement-=0x04000000
            if (displacement+(0 if word & 2 else textaddr+offset)) & 0xffffffff == dest:
                print(f"{textaddr+offset:08x} -> {dest:08x}")
    else:
        addr=int(target,16)
        for start,file_size,file_offset in segments:
            if start<=addr<start+file_size:
                offset=addr-start+file_offset
                print(f"{addr:08x} {data[offset:offset+160].split(bytes(1))[0]!r}")
                break
