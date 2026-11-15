#!/usr/bin/env python3
"""Check a .ko's undefined symbols against a kernel's symbol tables, and print
the module's declared kernel version dependency.  Usage: chkimports.py KERNEL KO..."""
import struct, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ctfdump

def syms(path, types):
    f = open(path, "rb").read()
    secs = list(ctfdump.elf_sections(f).values())
    out = {}
    for s in secs:
        if s[1] in types:
            st = secs_by_idx(f)[s[6]]
            for i in range(s[5] // 24):
                nm, info, oth, shn, val, sz = struct.unpack_from("<IBBHQQ", f, s[4] + i * 24)
                n = f[st[4] + nm:f.index(b"\0", st[4] + nm)].decode()
                out.setdefault(n, []).append((shn, val, sz))
    return f, out

def secs_by_idx(f):
    shoff = struct.unpack_from("<Q", f, 0x28)[0]
    es, n, _ = struct.unpack_from("<HHH", f, 0x3a)
    return [struct.unpack_from("<IIQQQQIIQQ", f, shoff + i * es) for i in range(n)]

_, k = syms(sys.argv[1], (2, 11))
kdef = {n for n, v in k.items() if any(s[0] != 0 for s in v)}
for ko in sys.argv[2:]:
    f, m = syms(ko, (2,))
    undef = sorted(n for n, v in m.items() if n and all(s[0] == 0 for s in v))
    miss = [u for u in undef if u not in kdef]
    # kernel version dependency: struct mod_depend {int min, pref, max} in
    # _<mod>_depend_on_kernel
    dep = [n for n in m if n.endswith("_depend_on_kernel")]
    secs = secs_by_idx(f)
    vers = []
    for n in dep:
        shn, val, sz = m[n][0]
        sh = secs[shn]
        vers.append((n, struct.unpack_from("<iii", f, sh[4] + val)))
    print(f"{os.path.basename(ko)}: {len(undef)} imports, missing from kernel: {miss or 'none'}; "
          f"kernel dependency: {vers}")
