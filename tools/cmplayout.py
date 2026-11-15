#!/usr/bin/env python3
"""Compare struct layouts between a FreeBSD kernel's CTF and the headers a
module is built against.

Usage: cmplayout.py KERNEL ZIG SYS GEN INC STRUCT[,...]
  INC is a ;-separated list of #include lines (or @file with one per line).
For every member the kernel CTF knows, a probe file computes offsetof() with
the build headers; the results are compared bit for bit.
"""
import os, re, struct, subprocess, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ctfdump

kernel, zig, sysdir, gen, inc, structs = sys.argv[1:7]
structs = structs.split(",")
incs = open(inc[1:]).read().splitlines() if inc.startswith("@") else inc.split(";")

types = ctfdump.load(kernel)
ctf = {}
for kind, name, size, mem in types.values():
    if kind in (6, 7) and name in structs and mem and name not in ctf:
        ctf[name] = (size, [(m, o) for m, o in mem if m])

cf = ["--target=x86_64-unknown-freebsd16.0", "-c", "-O0", "-D_KERNEL", "-DKLD_MODULE",
      "-nostdinc", "-include", f"{gen}/opt_global.h", f"-I{gen}", f"-I{sysdir}",
      f"-I{sysdir}/contrib/ck/include", "-ffreestanding", "-mno-sse", "-msoft-float",
      "-std=gnu99", "-w"]

def probe(items):
    """items: list of (key, expr). Returns dict key->value or (None, errors)."""
    src = "\n".join(f"#include {i}" if not i.startswith("#") else i for i in incs) + "\n"
    src += "#include <sys/stddef.h>\n"
    for n, (k, e) in enumerate(items):
        src += f"const unsigned long __probe_{n} = {e};\n"
    d = tempfile.mkdtemp()
    c, o = f"{d}/p.c", f"{d}/p.o"
    open(c, "w").write(src)
    r = subprocess.run([zig, "clang", *cf, "-o", o, c], capture_output=True, text=True)
    if r.returncode:
        return None, r.stderr
    f = open(o, "rb").read()
    secs = ctfdump.elf_sections(f)
    sym, strtab = secs[".symtab"], secs[".strtab"]
    shoff = struct.unpack_from("<Q", f, 0x28)[0]
    es = struct.unpack_from("<H", f, 0x3a)[0]
    out = {}
    for i in range(sym[5] // 24):
        nm, info, oth, shn, val, sz = struct.unpack_from("<IBBHQQ", f, sym[4] + i * 24)
        name = f[strtab[4] + nm:f.index(b"\0", strtab[4] + nm)].decode()
        if name.startswith("__probe_"):
            sh = struct.unpack_from("<IIQQQQIIQQ", f, shoff + shn * es)
            out[items[int(name[8:])][0]] = struct.unpack_from("<Q", f, sh[4] + val)[0]
    return out, None

items, skipped = [], []
for s, (size, mem) in ctf.items():
    items.append(((s, None), f"sizeof(struct {s})"))
    for m, off in mem:
        if off % 8:
            skipped.append(f"{s}.{m} (bit-field)")
            continue
        items.append(((s, m), f"__offsetof(struct {s}, {m})"))

missing = []
while True:
    res, err = probe(items)
    if res is not None:
        break
    bad = set()
    for line in err.splitlines():
        mm = re.search(r"no member named '(\w+)' in 'struct (\w+)'", line)
        if mm:
            bad.add((mm.group(2), mm.group(1)))
        mm = re.search(r"cannot compute offset of bit-field '(\w+)'", line)
        if mm:
            bf = {k for k, e in items if k[1] == mm.group(1)}
            skipped.extend(f"{k[0]}.{k[1]} (bit-field)" for k in bf)
            items = [(k, e) for k, e in items if k not in bf]
            bad.add(("", ""))
        if "incomplete" in line or "unknown type" in line:
            mm = re.search(r"struct (\w+)", line)
            if mm:
                bad |= {k for k, e in items if k[0] == mm.group(1)}
    if not bad and "bit-field" not in err:
        sys.exit("probe failed:\n" + err[:3000])
    bad.discard(("", ""))
    for k in bad:
        missing.append(f"{k[0]}.{k[1]}" if k[1] else f"struct {k[0]}")
    items = [(k, e) for k, e in items if k not in bad and (k[0], None) not in bad]

diffs = 0
for s, (size, mem) in ctf.items():
    if (s, None) not in res:
        continue
    if res[(s, None)] != size:
        print(f"DIFF sizeof(struct {s}): kernel {size} build {res[(s, None)]}"); diffs += 1
    for m, off in mem:
        if (s, m) in res and res[(s, m)] * 8 != off:
            print(f"DIFF {s}.{m}: kernel bit {off} build bit {res[(s, m)] * 8}"); diffs += 1
for s in structs:
    if s not in ctf:
        print(f"note: struct {s} not in kernel CTF")
for m in missing:
    print(f"MISSING in build headers: {m}"); diffs += 1
checked = len(res)
print(f"checked {checked} sizes/offsets across {len(ctf)} structs, {diffs} difference(s)"
      + (f", {len(skipped)} bit-fields skipped" if skipped else ""))
