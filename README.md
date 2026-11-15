# velo540pfsense: Intel I354 ports on a VeloCloud Edge 540 running pfSense

`vcigb` is FreeBSD's e1000/igb driver with VeloCloud Edge 520/540 board support
added. It is built as a standalone kernel module (`vcigb.ko`) and runs alongside
the `em`/`igb` driver compiled into pfSense's kernel. It claims only
`8086:1f41` (I354 SGMII), and only when SMBIOS reports a VeloCloud EDGE520/540.
On those boards it outbids the built-in igb driver, which fails on these ports
with `Setup of Shared code failed, error -2`. Each port must also carry the NVM
signature `"Vc"/"5X"` in words 0x06/0x07, or the driver leaves it alone.

Interface-to-front-panel mapping, confirmed on the box:

| PCI     | Interface | Front panel | Role                                                       |
|---------|-----------|-------------|------------------------------------------------------------|
| 00:14.0 | vcigb0    | LAN5-8      | switch A (88E6176) uplink, SerDes forced 1000/full         |
| 00:14.1 | vcigb1    | LAN1-4      | switch B (88E6176) uplink, SerDes forced 1000/full         |
| 00:14.2 | vcigb2    | GE1         | 88E1514 at addr 0 on GPIO 13/14 bit-bang MDIO              |
| 00:14.3 | vcigb3    | GE2         | 88E1514 at addr 1 on GPIO 13/14 bit-bang MDIO              |
| 04:00.0 | igb0      | SFP1        | I350, stock igb driver                                     |
| 04:00.1 | igb1      | SFP2        | I350, stock igb driver                                     |

The switches run unmanaged. The driver puts switch ports 0-4 straight into
Forwarding, so the 88E6176 does no spanning tree of its own. Ports 5/6
(RGMII cross-links) are Disabled.

## Status

Tested on one EDGE540 (board rev 2.8) running pfSense CE 2.9.0:
* All four I354 ports attach at boot.
* Traffic passes through the LAN switches.
* The watchdog is held off by `ichwd`.

GE1/GE2 have only been seen to link up; traffic on them is untested. Board
revision 1.x and the EDGE520 are untested.

## Installing (pfSense 2.9.0)

Prebuilt modules for pfSense 2.9.0 (kernel `__FreeBSD_version` 1600018) are
attached to the GitHub release. **They only work on that exact kernel.** Check
yours with `sysctl kern.osreldate`, which must print `1600018`.

On the box, as root:

```sh
fetch https://github.com/danic-jrm/velo540pfsense/releases/download/v0.1.0-pfsense-2.9.0/vcigb.ko
fetch https://github.com/danic-jrm/velo540pfsense/releases/download/v0.1.0-pfsense-2.9.0/ichwd.ko
fetch https://github.com/danic-jrm/velo540pfsense/releases/download/v0.1.0-pfsense-2.9.0/SHA256SUMS
sha256 -c "$(awk '$2=="vcigb.ko"{print $1}' SHA256SUMS)" vcigb.ko
sha256 -c "$(awk '$2=="ichwd.ko"{print $1}' SHA256SUMS)" ichwd.ko
install -m 444 vcigb.ko ichwd.ko /boot/modules/
kldxref /boot/modules
cat >> /boot/loader.conf.local <<'CONF'
ichwd_load="YES"
vcigb_load="YES"
CONF
```

On the next boot the ports appear as `vcigb0`-`vcigb3`. Assign them in the
pfSense GUI.

**About the watchdog:** the EDGE540 BIOS arms the Intel TCO watchdog, and
pfSense 2.9.0 ships no `ichwd` driver. Without `ichwd_load="YES"` the box
reboots about 2 minutes after every boot.

**Before upgrading pfSense:** both modules must be rebuilt for the new kernel.
A module built for an older kernel may still load on a newer one in the same
release series and crash it. Do the upgrade in a new ZFS boot environment
(`bectl create -r`, then `bectl activate -t` for a one-time trial boot). Then
a failed boot falls back on its own, either through the watchdog or a panic
reboot (set `debug.debugger_on_panic="0"`).

## What the driver does on attach

* **Funcs 0/1:** no PHY probe. Media is internal SerDes and the PCS is forced
  to 1000/full. Next, the 88E6176 is brought up as a plain switch over the
  function's own MDIO pins, using the same sequence as velo540openwrt's
  `velo540-switch` init script:
  1. Port-4 SerDes: autoneg off and power-up.
  2. Port 4: PHYDetect off and MAC forced to 1000/full.
  3. Internal PHYs 0-3: reset.
  4. Ports 0-4 set to Forwarding, ports 5/6 Disabled.
* **Funcs 2/3:** the first of the two to attach does three things:
  1. Muxes SoC GPIO 11-14 to GPIO mode.
  2. Pulses PCA9557@0x18 pin 4 over bit-banged I2C (GPIO 11/12) to take the
     88E1514s out of reset. Only pin 4 is changed.
  3. Routes PHY register access to the bit-banged MDIO bus.

  It then polls until both PHYs answer on MDIO. If they don't within 2 s,
  it runs a bus diagnostic (GPIO registers, MDIO idle level, an address
  scan at two bus speeds) and pulses the reset again, up to 3 times.

  Then the stock 88E1512/1514 code takes over. The WAN LEDs are set as on
  the vendor firmware.

Test aid: running `kenv hw.vcigb.restore_on_unload=1` before `kldload` makes
`kldunload` put the WAN PHYs back into their power-on reset and GPIO 13/14
back to native mode. The next load then starts from the same state as a
cold boot. `hw.vcigb.mdio_delay` sets the MDIO half-period in µs (default 1).

Board rev 1.x (with a different GPIO pinout) is refused for funcs 2/3.

## Files

The driver must be built for the exact kernel it runs on, so there is one
tree per kernel version (`__FreeBSD_version`):

| Directory   | Kernel | Base source |
|-------------|--------|-------------|
| `k1600004/` | 1600004: Netgate installer kernel `installer-RELENG_v1_1_1` (Dec 2025) | pfSense FreeBSD-src `cf39b51d36d8` |
| `k1600018/` | 1600018: pfSense 2.9.0 `RELENG_2_9_0-n256132-d8e3138ecf52` (Aug 2026) | upstream FreeBSD `7386c0675c89` (last commit at 1600018) |

Each tree contains:
* `e1000.orig/`: the unmodified base.
* `e1000/`: patched. `e1000_vc.[ch]` is new; the other changes are small hooks
  in `e1000_82575.c`, `e1000_hw.h` and `if_em.c`.
* `vcigb.diff`: the full change against `e1000.orig`.
* `build/` (created by `build-zig.sh`, not kept in git): `vcigb.ko` and
  `ichwd.ko`. Releases carry the prebuilt files.

Other files:
* `build-zig.sh`: cross-builds with zig's clang/lld. Set `SYS`, `ZIG`, `GEN`
  and `K` as described in the script's header.
* `Makefile`: native FreeBSD build, `make SYSDIR=/usr/src/sys K=k1600018` (default `K` is `k1600018`).
* `tools/ctfdump.py`: dumps struct layouts from a kernel's CTF (`.SUNW_ctf`).
* `tools/cmplayout.py`: compares those layouts with the build headers.
* `tools/chkimports.py`: checks a module's imports against a kernel and prints
  its declared kernel version.

## How the modules are checked against a kernel

Without the kernel's exact source, a module is accepted only if:
1. Every symbol it imports is exported by the target kernel
   (`tools/chkimports.py`).
2. Every shared structure has the same layout as in the kernel's CTF type
   data (`tools/cmplayout.py`). This covers iflib, sysctl, locks, module
   metadata, mbufs, ifmedia, threads and pcpu.

Results for the 2.9.0 kernel:
* `vcigb.ko`: 148 imports, all exported.
* `ichwd.ko`: 39 imports, all exported.
* Layouts: 548 sizes and offsets across 47 structs, no differences
  (8 bit-fields skipped).
* Neither `ichwd` nor `i6300esbwd` is built into that kernel.

Kernel version rule: FreeBSD loads a module on any kernel from its build
version up to x.99999. So a 1600004 build *will* load on a 1600018 kernel,
and must never be left in `/boot/modules` across a kernel upgrade.

## Testing (needs root, from the console)

```sh
# from the workstation, after building
scp k1600018/build/vcigb.ko root@<pfsense>:/tmp/
# on the box, as root (console / shell option 8)
kldload /tmp/vcigb.ko
dmesg | tail -40
ifconfig -l; ifconfig vcigb0; ifconfig vcigb2
sysctl dev.vcigb
```

Expected in dmesg:

* `VeloCloud edge5x0: switch A uplink, forced 1000/full SerDes`
* `88E6176 (id 0x176x) switch A up, port 4 status 0x....`
* `SoC GPIO @0x...: USE_SEL ...`
* `PCA9557@0x18: output ... config ..., resetting WAN PHYs`
* `VeloCloud edge5x0: GE1, 88E1514 at GPIO MDIO address 0`

On success, `vcigb0..3` attach and the GE ports show link when cabled.

### Debugging MDIO

Each port has two sysctls. Funcs 0/1 use the switch MDIO; funcs 2/3 use the
GPIO bus.

```sh
sysctl dev.vcigb.0.vc_mdio_sel=$(( (0x10 << 8) | 3 ))   # switch port 0, reg 3
sysctl dev.vcigb.0.vc_mdio_val                          # -> 0x176x switch ID
sysctl dev.vcigb.2.vc_mdio_sel=$(( (0 << 8) | 2 ))      # 88E1514 addr 0, PHYID1
sysctl dev.vcigb.2.vc_mdio_val                          # -> 0x141
```

### Making it persistent

See [Installing](#installing-pfsense-290). `kldunload vcigb` detaches the
module again.

## Licence

BSD-2-Clause, see `LICENSE`. Third-party code is listed in `NOTICE`; the FreeBSD e1000 sources keep their own
Intel/FreeBSD BSD licences.
