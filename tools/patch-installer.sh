#!/bin/sh
#
# patch-installer.sh: build a VeloCloud Edge 520/540 installer image from a
# stock Netgate pfSense installer image.
#
# The patched installer, on a VeloCloud Edge only:
#   * uses COM2 (0x2F8) as its console, attaching the uarts through ISA hints
#     (coreboot's ACPI tables list no serial ports);
#   * loads ichwd (already in the image) so the BIOS-armed TCO watchdog does
#     not reboot it, and vcigb so the I354 ports work during the install.
# After installing, it also sets up the target:
#   * COM2 settings in /boot/loader.conf.local, serial console enabled;
#   * ichwd.ko and vcigb.ko in /boot/modules, loaded at boot, but only if the
#     installed kernel is exactly the one the modules were built for.
# On other hardware the image behaves like the stock one.
#
# Run as root on FreeBSD (uses mdconfig, mount, fsck_ffs, kldxref).
#
# usage: patch-installer.sh STOCK.img OUT.img INSTALLER_MODDIR TARGET_MODDIR
#   INSTALLER_MODDIR  vcigb.ko built for the installer kernel
#   TARGET_MODDIR     ichwd.ko and vcigb.ko built for the pfSense kernel
#                     that the installer installs
#
# The kernels are checked by ident string; override with INSTALLER_IDENT and
# TARGET_IDENT when building for other kernels.

set -eu

INSTALLER_IDENT="${INSTALLER_IDENT:-installer-RELENG_v1_1_1-n256503-329d700f627c}"
TARGET_IDENT="${TARGET_IDENT:-RELENG_2_9_0-n256132-d8e3138ecf52}"
SHARE="/usr/local/share/velo540pfsense"
MARK="velo540pfsense"
POST="usr/local/libexec/installer/backend/pfSense-post-install"

die() {
	echo "patch-installer: $*" >&2
	exit 1
}

[ $# -eq 4 ] || die "usage: patch-installer.sh STOCK.img OUT.img INSTALLER_MODDIR TARGET_MODDIR"
STOCK="$1"; OUT="$2"; IMODS="$3"; TMODS="$4"
[ "$(id -u)" -eq 0 ] || die "must run as root"
[ "$(uname -s)" = "FreeBSD" ] || die "must run on FreeBSD"
[ -f "${STOCK}" ] || die "no such image: ${STOCK}"
[ ! -e "${OUT}" ] || die "${OUT} already exists"
for f in "${IMODS}/vcigb.ko" "${TMODS}/vcigb.ko" "${TMODS}/ichwd.ko"; do
	[ -f "${f}" ] || die "missing ${f}"
done

MD=""
MNT=""
OK=""
cleanup() {
	[ -n "${MNT}" ] && { umount "${MNT}" 2>/dev/null || true; rmdir "${MNT}"; }
	[ -n "${MD}" ] && mdconfig -d -u "${MD}"
	[ -z "${OK}" ] && rm -f "${OUT}"
	return 0
}
trap cleanup EXIT INT TERM

echo "copying ${STOCK} -> ${OUT}"
cp "${STOCK}" "${OUT}"
MD="$(mdconfig -a -t vnode -f "${OUT}")"
DEV="/dev/${MD}s2a"
[ "$(fstyp "${DEV}")" = "ufs" ] || die "${DEV} is not UFS"
MNT="$(mktemp -d -t velo540)"
mount -t ufs "${DEV}" "${MNT}"

LUA="${MNT}/boot/loader.conf.lua"
grep -q "${MARK}" "${LUA}" "${MNT}/${POST}" && die "image is already patched"
grep -qaF "${INSTALLER_IDENT}" "${MNT}/boot/kernel/kernel" ||
    die "installer kernel is not ${INSTALLER_IDENT}"
[ -f "${MNT}/boot/kernel/ichwd.ko" ] || die "installer has no /boot/kernel/ichwd.ko"

# insert_before FILE ANCHOR TEXT: insert TEXT before the one line equal to ANCHOR.
insert_before() {
	[ "$(grep -cxF -- "$2" "$1")" -eq 1 ] || die "$1: anchor not found exactly once: $2"
	printf '%s\n' "$3" > "${MNT}.ins"
	awk -v anchor="$2" -v ins="${MNT}.ins" '
	    $0 == anchor { while ((getline l < ins) > 0) print l }
	    { print }' "$1" > "$1.new"
	rm -f "${MNT}.ins"
	cat "$1.new" > "$1"
	rm -f "$1.new"
}

# 1. Installer: COM2 console, ichwd and vcigb on a VeloCloud Edge.
echo "installer: loader.conf.lua, /boot/modules/vcigb.ko"
insert_before "${LUA}" 'comconsole_speed="115200"' \
'if string.sub(_ENV["smbios.system.maker"] or "", 1, 9) == "VeloCloud" then -- velo540pfsense
	print("VeloCloud Edge detected.")
	_ENV["boot_serial"]="YES"
	_ENV["console"]="comconsole"
	_ENV["comconsole_port"]="0x2F8"
	_ENV["hint.uart.0.at"]="isa"
	_ENV["hint.uart.0.flags"]="0x00"
	_ENV["hint.uart.1.at"]="isa"
	_ENV["hint.uart.1.flags"]="0x10"
	_ENV["ichwd_load"]="YES"
	_ENV["vcigb_load"]="YES"
end
'
install -m 444 "${IMODS}/vcigb.ko" "${MNT}/boot/modules/vcigb.ko"
kldxref "${MNT}/boot/modules"

# 2. Target: modules, kernel ident and the setup script, run from post-install.
echo "target: ${SHARE}"
mkdir -p "${MNT}${SHARE}/modules"
install -m 444 "${TMODS}/ichwd.ko" "${TMODS}/vcigb.ko" "${MNT}${SHARE}/modules/"
echo "${TARGET_IDENT}" > "${MNT}${SHARE}/kernel-ident"
cat > "${MNT}${SHARE}/post-install.sh" << 'SCRIPT'
#!/bin/sh
# velo540pfsense: set up a freshly installed pfSense on a VeloCloud Edge
# 520/540. Run by pfSense-post-install with the target's root directory.
# Exits 1 without doing anything on other hardware.

# kldxref needs an absolute path.
destdir="$(cd "${1:?usage: post-install.sh DESTDIR}" && pwd)" || exit 0
share="/usr/local/share/velo540pfsense"

case "$(/bin/kenv -q smbios.planar.maker)/$(/bin/kenv -q smbios.planar.product)" in
VeloCloud*/EDGE520|VeloCloud*/EDGE540)
	;;
*)
	exit 1
	;;
esac

conf="${destdir}/boot/loader.conf.local"
echo "VeloCloud Edge: serial console on COM2."
/bin/cat >> "${conf}" << EOF
comconsole_port="0x2F8"
hint.uart.0.at="isa"
hint.uart.0.flags="0x00"
hint.uart.1.at="isa"
hint.uart.1.flags="0x10"
EOF

ident="$(/bin/cat "${share}/kernel-ident")"
if /usr/bin/grep -qaF "${ident}" "${destdir}/boot/kernel/kernel"; then
	/bin/mkdir -p "${destdir}/boot/modules"
	/usr/bin/install -m 444 "${share}/modules/ichwd.ko" \
	    "${share}/modules/vcigb.ko" "${destdir}/boot/modules/"
	/usr/sbin/kldxref "${destdir}/boot/modules"
	/bin/cat >> "${conf}" << EOF
ichwd_load="YES"
vcigb_load="YES"
EOF
	echo "VeloCloud Edge: installed ichwd.ko and vcigb.ko."
else
	echo "WARNING: the installed kernel is not ${ident}."
	echo "WARNING: ichwd.ko and vcigb.ko were NOT installed. Without ichwd"
	echo "WARNING: the system reboots about 2 minutes after each boot."
fi
exit 0
SCRIPT
chmod 555 "${MNT}${SHARE}/post-install.sh"

echo "target: ${POST}"
insert_before "${MNT}/${POST}" '	# Setup the legacy serial console if necessary.' \
'	# velo540pfsense: VeloCloud Edge 520/540 (COM2 console, ichwd, vcigb).
	if /bin/sh '"${SHARE}"'/post-install.sh "${_destdir}"; then
		[ -z "${SERIAL_CONSOLE}" ] && SERIAL_CONSOLE="serial"
	fi
'
sh -n "${MNT}/${POST}" || die "${POST}: syntax error after patching"
sh -n "${MNT}${SHARE}/post-install.sh" || die "post-install.sh: syntax error"
/usr/libexec/flua -e "assert(loadfile('${LUA}'))" || die "loader.conf.lua: syntax error"

umount "${MNT}"
rmdir "${MNT}"
MNT=""
echo "fsck_ffs -fn ${DEV}"
fsck_ffs -fn "${DEV}" || die "fsck_ffs reported errors"
mdconfig -d -u "${MD}"
MD=""
OK=1
sha256 "${OUT}"
echo "done; write ${OUT} to a USB stick"
