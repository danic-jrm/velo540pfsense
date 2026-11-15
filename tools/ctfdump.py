#!/usr/bin/env python3
"""Dump struct/union layouts from a FreeBSD kernel's .SUNW_ctf (CTF v2/v3).
Usage: ctfdump.py KERNEL STRUCT [STRUCT...]  -> prints 'struct size' and 'struct.member offset_bits'"""
import struct, sys, zlib

def elf_sections(f):
    shoff = struct.unpack_from("<Q", f, 0x28)[0]
    es, n, shstr = struct.unpack_from("<HHH", f, 0x3a)
    secs = [struct.unpack_from("<IIQQQQIIQQ", f, shoff + i * es) for i in range(n)]
    base = secs[shstr][4]
    name = lambda o: f[base + o:f.index(b"\0", base + o)].decode()
    return {name(s[0]): s for s in secs}

def load(path):
    f = open(path, "rb").read()
    secs = elf_sections(f)
    c = secs[".SUNW_ctf"]; ctf = f[c[4]:c[4] + c[5]]
    strtab = secs[".strtab"]; ext = f[strtab[4]:strtab[4] + strtab[5]]
    magic, ver, flags = struct.unpack_from("<HBB", ctf, 0)
    assert magic == 0xcff1, hex(magic)
    hdr = struct.unpack_from("<8I", ctf, 4)
    lbl, objt, func, typeoff, stroff, strlen = hdr[2:]
    data = ctf[36:]
    if flags & 1:
        data = zlib.decompress(data)
    strs = data[stroff:stroff + strlen]
    def s(off):
        if ver >= 3:
            tab, off = (ext if off & 0x80000000 else strs), off & 0x7fffffff
        else:
            tab, off = (ext if off & 0x80000000 else strs), off & 0x7fffffff
        return tab[off:tab.index(b"\0", off)].decode(errors="replace")
    types = {}; p = typeoff; tid = 1
    v3 = ver >= 3
    while p < stroff:
        if v3:
            name, info, size = struct.unpack_from("<III", data, p); p += 12
            kind, vlen = (info >> 26) & 0x3f, info & 0xffffff
            if size == 0xffffffff:
                hi, lo = struct.unpack_from("<II", data, p); p += 8; size = (hi << 32) | lo
        else:
            name, info, size = struct.unpack_from("<IHH", data, p); p += 8
            kind, vlen = (info >> 11) & 0x1f, info & 0x3ff
            if size == 0xffff:
                hi, lo = struct.unpack_from("<II", data, p); p += 8; size = (hi << 32) | lo
        mem = None
        if kind in (1, 2): p += 4
        elif kind == 4: p += 12 if v3 else 8
        elif kind == 5:
            p += vlen * (4 if v3 else 2)
            if not v3 and vlen & 1: p += 2
        elif kind in (6, 7):
            mem = []
            thresh = (1 << 29) if v3 else (1 << 13)
            for _ in range(vlen):
                if size >= thresh:
                    if v3:
                        mn, mt, ohi, olo = struct.unpack_from("<IIII", data, p); p += 16
                    else:
                        mn, mt, _, ohi, olo = struct.unpack_from("<IHHII", data, p); p += 16
                    off = (ohi << 32) | olo
                else:
                    if v3:
                        mn, mt, off = struct.unpack_from("<III", data, p); p += 12
                    else:
                        mn, mt, off = struct.unpack_from("<IHH", data, p); p += 8
                mem.append((s(mn), off))
        elif kind == 8: p += vlen * 8
        types[tid] = (kind, s(name), size, mem); tid += 1
    return types

if __name__ == "__main__":
    types = load(sys.argv[1])
    want = set(sys.argv[2:])
    seen = set()
    for kind, name, size, mem in types.values():
        if kind in (6, 7) and name in want and mem and name not in seen:
            seen.add(name)
            print(f"{name} size {size}")
            for m, off in mem:
                if m: print(f"{name}.{m} {off}")
    for w in sorted(want - seen):
        print(f"{w} MISSING", file=sys.stderr)
