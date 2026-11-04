#!/usr/bin/env python3
# parse objdump output, resolve adrp+add pairs to strings from the kernel binary
import re, struct, sys

asm_path, kernel_path = sys.argv[1], sys.argv[2]
data = open(kernel_path, 'rb').read()

# build segment map for v2f
magic, cputype, cpusub, filetype, ncmds = struct.unpack_from('<IiiII', data, 0)
off = 32; segs = []
for i in range(ncmds):
    cmd, size = struct.unpack_from('<II', data, off)
    if cmd == 0x19:
        vmaddr, vmsize, fileoff, filesize = struct.unpack_from('<QQQQ', data, off+24)
        segs.append((vmaddr, vmsize, fileoff))
    off += size
def v2f(v):
    for va, vs, fo in segs:
        if va <= v < va+vs: return fo + (v-va)
    return None
def getstr(v):
    f = v2f(v)
    if f is None: return None
    end = data.find(b'\x00', f)
    if end < 0 or end - f > 200 or end == f: return None
    s = data[f:end]
    if all(32 <= c < 127 or c in (9,10) for c in s): return s.decode()
    return None

bases = {}
for ln in open(asm_path):
    m = re.match(r'\s*[0-9a-f]+:.*\badrp\s+(x\d+), (0x[0-9a-f]+)', ln)
    if m: bases[m.group(1)] = int(m.group(2), 16); continue
    m = re.match(r'\s*[0-9a-f]+:.*\badd\s+(x\d+), (?:x\d+), #(0x[0-9a-f]+|\d+)', ln)
    if m and m.group(1) in bases:
        v = bases[m.group(1)] + int(m.group(2), 0)
        s = getstr(v)
        if s: print(f'{ln.split(":")[0].strip()}: {s}')
