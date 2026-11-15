#!/bin/sh
# Cross-build vcigb.ko on Linux with zig's clang/lld (no FreeBSD host needed).
#   SYS = pfSense FreeBSD-src sys/ tree matching the target kernel
#   ZIG = path to the zig binary (e.g. from the PyPI "ziglang" wheel)
#   GEN = dir with opt_*.h generated from the target's `sysctl kern.conftxt`
#   K   = per-kernel dir holding e1000/ (patched sources); output goes to $K/build
# Usage: SYS=... ZIG=... GEN=... K=k1600018 ./build-zig.sh
set -eu
: "${SYS:?}" "${ZIG:?}" "${GEN:?}" "${K:?}"
HERE=$(cd "$(dirname "$0")" && pwd)
K=$(cd "$HERE" && cd "$K" && pwd)
OBJ=$(mktemp -d)
trap 'rm -rf "$OBJ"' EXIT

ln -sfn "$SYS/amd64/include" "$GEN/machine"
ln -sfn "$SYS/x86/include" "$GEN/x86"
(cd "$GEN" && for m in kern/device_if.m kern/bus_if.m dev/pci/pci_if.m net/ifdi_if.m; do
	awk -f "$SYS/tools/makeobjops.awk" "$SYS/$m" -h; done)

CF="--target=x86_64-unknown-freebsd16.0 -c -O2 -pipe -fno-strict-aliasing
 -D_KERNEL -DKLD_MODULE -nostdinc -include $GEN/opt_global.h
 -I$K/e1000 -I$GEN -I$SYS -I$SYS/contrib/ck/include
 -fno-common -fno-omit-frame-pointer -mno-omit-leaf-frame-pointer
 -mcmodel=kernel -mno-red-zone -mno-mmx -mno-sse -msoft-float -mno-aes -mno-avx
 -fno-asynchronous-unwind-tables -ffreestanding -fwrapv -fstack-protector
 -std=gnu99 -Wall -Wmissing-prototypes -Wno-pointer-sign"

for f in if_em em_txrx igb_txrx e1000_osdep e1000_api e1000_base e1000_phy \
    e1000_nvm e1000_mac e1000_manage e1000_vf e1000_mbx e1000_i210 \
    e1000_80003es2lan e1000_ich8lan e1000_82571 e1000_82575 e1000_82540 \
    e1000_82542 e1000_82541 e1000_82543 e1000_vc; do
	"$ZIG" clang $CF -o "$OBJ/$f.o" "$K/e1000/$f.c"
done

mkdir -p "$K/build"
"$ZIG" ld.lld -m elf_x86_64_fbsd -d -warn-common --build-id=sha1 -S \
    -T "$SYS/conf/ldscript.kmod.amd64" -r -o "$K/build/vcigb.ko" "$OBJ"/*.o
echo "built $K/build/vcigb.ko"

# ichwd (stock FreeBSD watchdog driver; pfSense 2.9.0 ships none)
if [ -f "$SYS/dev/ichwd/ichwd.c" ]; then
	mkdir -p "$OBJ/wd"
	(cd "$GEN" && [ -f isa_if.h ] || awk -f "$SYS/tools/makeobjops.awk" "$SYS/isa/isa_if.m" -h)
	for f in $(sed -n 's/^SRCS[+]*=[[:space:]]*//p' "$SYS/modules/ichwd/Makefile" | tr ' ' '\n' | grep '\.c$'); do
		"$ZIG" clang $CF -I"$SYS/dev/ichwd" -o "$OBJ/wd/${f%.c}.o" "$SYS/dev/ichwd/$f"
	done
	"$ZIG" ld.lld -m elf_x86_64_fbsd -d -warn-common --build-id=sha1 -S \
	    -T "$SYS/conf/ldscript.kmod.amd64" -r -o "$K/build/ichwd.ko" "$OBJ"/wd/*.o
	echo "built $K/build/ichwd.ko"
fi
