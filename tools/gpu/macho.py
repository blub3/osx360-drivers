# Minimal 32-bit big-endian PPC Mach-O reader: resolve addresses to C strings and CFStrings.
import struct, sys
class MachO:
    def __init__(s, path):
        d = s.d = open(path, 'rb').read()
        magic, cpu, sub, ft, ncmds, size, flags = struct.unpack('>7I', d[:28])
        assert magic == 0xfeedface, hex(magic)
        s.secs = []; off = 28
        for _ in range(ncmds):
            cmd, cs = struct.unpack('>2I', d[off:off+8])
            if cmd == 1:
                nsects = struct.unpack('>I', d[off+48:off+52])[0]; so = off + 56
                for i in range(nsects):
                    sn, sg = d[so:so+16].rstrip(b'\0').decode(), d[so+16:so+32].rstrip(b'\0').decode()
                    addr, sz, fo = struct.unpack('>3I', d[so+32:so+44])
                    s.secs.append((sg, sn, addr, sz, fo)); so += 68
            off += cs
    def sec(s, a):
        for sg, sn, addr, sz, fo in s.secs:
            if addr <= a < addr + sz: return sg, sn, a - addr + fo
        return None
    def u32(s, a):
        r = s.sec(a); return struct.unpack('>I', s.d[r[2]:r[2]+4])[0] if r else None
    def cstr(s, a):
        r = s.sec(a)
        if not r: return None
        e = s.d.index(b'\0', r[2]); return s.d[r[2]:e].decode('latin1')
    def describe(s, a):
        r = s.sec(a)
        if not r: return '?'
        if r[1] == '__cfstring' or (s.u32(a + 4) in (0x7c8, 0x7d0) and s.sec(s.u32(a + 8) or 0)):
            return 'CFSTR(%r)' % s.cstr(s.u32(a + 8))
        if r[1] in ('__cstring', '__const'): return '%s %r' % (r[1], s.cstr(a))
        return '%s,%s +0x%x' % (r[0], r[1], a)
if __name__ == '__main__':
    m = MachO(sys.argv[1])
    for x in sys.argv[2:]: print(x, m.describe(int(x, 16)))
