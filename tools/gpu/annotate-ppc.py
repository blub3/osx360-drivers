# Annotate otool -tV PPC output: resolve PIC-relative addresses (bcl/mflr base + addis/addi|lwz|lbz|stw).
import re, sys
import os; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); from macho import MachO
m = MachO(sys.argv[1]); regs = {}; base = None; pend = None
def s16(v): v &= 0xffff; return v - 0x10000 if v & 0x8000 else v
for line in open(sys.argv[2]):
    line = line.rstrip('\n'); note = ''
    f = line.split('\t')
    if len(f) >= 3:
        addr = int(f[0], 16); op = f[1]; args = f[2].split(';')[0].replace(' ', '')
        if op == 'bcl': base = addr + 4
        mm = re.match(r'(r\d+),lr$', args)
        if op == 'mfspr' and mm and base: regs[mm.group(1)] = base
        mm = re.match(r'(r\d+),(r\d+),(0x[0-9a-f]+|-?\d+)$', args)
        if op == 'addis' and mm and mm.group(2) in regs:
            regs[mm.group(1)] = (regs[mm.group(2)] + (s16(int(mm.group(3), 0)) << 16)) & 0xffffffff
        elif op == 'lis' and re.match(r'(r\d+),', args):
            mm2 = re.match(r'(r\d+),(0x[0-9a-f]+|-?\d+)$', args)
            if mm2: regs[mm2.group(1)] = (s16(int(mm2.group(2), 0)) << 16) & 0xffffffff
        elif op == 'addi' and mm and mm.group(2) in regs:
            a = (regs[mm.group(2)] + s16(int(mm.group(3), 0))) & 0xffffffff
            regs[mm.group(1)] = a; note = m.describe(a)
        else:
            mm = re.match(r'(r\d+),(0x[0-9a-f]+|-?\d+)\((r\d+)\)$', args)
            if mm and mm.group(3) in regs and op[0] in 'ls':
                a = (regs[mm.group(3)] + s16(int(mm.group(2), 0))) & 0xffffffff
                note = m.describe(a)
                if op.startswith('lwz') and m.sec(a) and m.sec(a)[1] in ('__nl_symbol_ptr','__la_symbol_ptr','__data','__const'):
                    note += ' -> ' + m.describe(m.u32(a) or 0)
    print(line + (('\t; ' + note) if note and note != '?' else ''))
