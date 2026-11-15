/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * VeloCloud Edge 520/540 ("edge5x0") board support for the e1000/igb driver.
 *
 * Board wiring of the four Intel I354 SGMII functions (8086:1f41), as
 * documented by the vendor's igb_vc.c and the velo540openwrt project:
 *
 *   func 0  SerDes -> Marvell 88E6176 switch A port 4 (LAN5-8).  No PHY:
 *           the link is forced to 1000/full.  The switch's SMI is on this
 *           function's own MDIO pins, single-chip addressing (switch
 *           register banks at MDIO addresses 0x10..0x1d).
 *   func 1  same, switch B (LAN1-4).
 *   func 2  SGMII -> Marvell 88E1514 PHY (GE1), MDIO address 0 on a
 *           bit-banged MDIO bus on SoC core-well GPIO 13 (MDIO) / 14 (MDC)
 *           (board rev 2.x).
 *   func 3  same, GE2, MDIO address 1.
 *
 * The 88E1514s are held in reset by pin 4 of a PCA9557 at I2C address
 * 0x18, on a bit-banged I2C bus on GPIO 11 (SDA) / 12 (SCL).
 *
 * The board is identified by I354 NVM words 0x06/0x07 = "Vc"/"5X".
 *
 * The register sequences (forced SerDes, 88E6176 bring-up, 88E1514 LEDs)
 * follow velo540openwrt's igb_vc.c patch and velo540-switch init script;
 * this file is a fresh implementation for FreeBSD, not a copy of that code.
 */

#include "e1000_api.h"
#include "e1000_vc.h"

#include <sys/kenv.h>
#include <sys/sysctl.h>
#include <machine/atomic.h>
#include <machine/cpufunc.h>

#define VC_NVM_WORD		0x06
#define VC_ID			0x5663	/* "Vc" */
#define VC_ID_EDGE5X0		0x3558	/* "5X" */

/* Atom C2000 LPC bridge (0:31.0) GPIO base, core-well GPIO 0..31 regs. */
#define C2000_LPC_GBA		0x48
#define C2000_GBA_MASK		0xff80
#define GPIO_USE_SEL		0x00	/* 1 = GPIO, 0 = native function */
#define GPIO_IO_SEL		0x04	/* 1 = input, 0 = output */
#define GPIO_LVL		0x08

#define VC_GPIO_SDA		11
#define VC_GPIO_SCL		12
#define VC_GPIO_MDIO		13
#define VC_GPIO_MDC		14
#define VC_GPIO_MASK		((1u << VC_GPIO_SDA) | (1u << VC_GPIO_SCL) | \
				 (1u << VC_GPIO_MDIO) | (1u << VC_GPIO_MDC))

#define VC_PCA9557_ADDR		0x18
#define PCA9557_OUTPUT		1
#define PCA9557_CONFIG		3	/* 1 = input */
#define VC_PCA9557_PHY_RST	(1u << 4)	/* active low */

/* 88E6176, single-chip addressing */
#define SW_PORT(p)		(0x10 + (p))
#define SW_GLOBAL2		0x1c
#define SW_G2_SMI_CMD		0x18
#define SW_G2_SMI_DATA		0x19
#define SW_SMI_BUSY		0x8000
#define SW_SMI_C22_WRITE	0x9400
#define SW_SMI_C22_READ		0x9800
#define SW_PORT_STATUS		0
#define SW_PORT_PCS_CTRL	1
#define SW_PORT_SWITCH_ID	3
#define SW_PORT_CTRL		4
#define SW_SERDES_DEV		0xf

static struct mtx vc_gpio_mtx;
MTX_SYSINIT(vc_gpio, &vc_gpio_mtx, "vcigb gpio", MTX_DEF);

static uint16_t vc_gbase;		/* 0 until vc_gpio_setup() succeeds */
static uint32_t vc_lvl;			/* GPIO_LVL shadow, see vc_gpio_setup() */
static volatile u_int vc_wan_reset_done;
static uint32_t vc_use_orig, vc_io_orig;	/* GPIO state before we touched it */

/*
 * Test aid: hw.vcigb.restore_on_unload=1 (kenv, before kldload) makes
 * module unload put the WAN PHYs back in reset (PCA9557 pin 4 as input,
 * as at power-on) and GPIO 11-14 back to their original mux, so the next
 * kldload sees the same state as a cold boot.
 */
static int vc_restore_on_unload = 0;
TUNABLE_INT("hw.vcigb.restore_on_unload", &vc_restore_on_unload);
static int vc_dbg_sel[4];		/* debug sysctl: (addr << 8) | reg */

/* MDIO half-period in us; loader/kenv tunable hw.vcigb.mdio_delay */
static int vc_mdio_hp = 1;
TUNABLE_INT("hw.vcigb.mdio_delay", &vc_mdio_hp);

static device_t
vc_dev(struct e1000_hw *hw)
{
	return (((struct e1000_osdep *)hw->back)->dev);
}

/*********************************************************************
 *  SoC GPIO (core well) - caller holds vc_gpio_mtx
 *********************************************************************/
static void
vc_pin_dir(int pin, bool out)
{
	uint32_t v;

	v = inl(vc_gbase + GPIO_IO_SEL);
	if (out)
		v &= ~(1u << pin);
	else
		v |= 1u << pin;
	outl(vc_gbase + GPIO_IO_SEL, v);
}

static void
vc_pin_set(int pin, int val)
{
	if (val)
		vc_lvl |= 1u << pin;
	else
		vc_lvl &= ~(1u << pin);
	outl(vc_gbase + GPIO_LVL, vc_lvl);
}

static int
vc_pin_get(int pin)
{
	return ((inl(vc_gbase + GPIO_LVL) >> pin) & 1);
}

static int
vc_gpio_setup(device_t dev)
{
	device_t lpc;
	uint32_t gba, use, io;
	char *ver;
	int rev2;

	if (vc_gbase != 0)
		return (0);

	/* Rev 1.x boards put the WAN MDIO bus on GPIO 11/12. Not supported. */
	ver = kern_getenv("smbios.planar.version");
	rev2 = (ver != NULL && ver[0] == '2');
	if (ver != NULL)
		freeenv(ver);
	if (!rev2) {
		device_printf(dev, "board revision is not 2.x, WAN PHY bus "
		    "pinout unknown\n");
		return (ENXIO);
	}

	lpc = pci_find_bsf(0, 31, 0);
	if (lpc == NULL) {
		device_printf(dev, "no LPC bridge at 0:31.0\n");
		return (ENXIO);
	}
	gba = pci_read_config(lpc, C2000_LPC_GBA, 4) & C2000_GBA_MASK;
	if (gba == 0) {
		device_printf(dev, "SoC GPIO base not set\n");
		return (ENXIO);
	}

	mtx_lock(&vc_gpio_mtx);
	if (vc_gbase == 0) {
		use = inl(gba + GPIO_USE_SEL);
		io = inl(gba + GPIO_IO_SEL);
		vc_use_orig = use;
		vc_io_orig = io;
		/*
		 * The Avoton GPIO block may not read back output levels
		 * (Linux gpio-ich keeps a software copy for this SoC), so
		 * GPIO_LVL is written from a shadow seeded once from the
		 * register.  Our four pins idle with level 0:
		 * MDC is a push-pull output, MDIO/SDA/SCL are released
		 * (input); SDA/SCL are driven low by switching to output.
		 */
		/*
		 * Mux the pins to GPIO first: IO_SEL/LVL writes to a pin
		 * still in native mode are dropped (seen at cold boot, where
		 * GPIO 13/14 start native and MDC stayed an input).  All four
		 * pins are inputs at power-on, so muxing first glitches nothing.
		 */
		if ((use & VC_GPIO_MASK) != VC_GPIO_MASK)
			outl(gba + GPIO_USE_SEL, use | VC_GPIO_MASK);
		vc_lvl = inl(gba + GPIO_LVL) & ~VC_GPIO_MASK;
		outl(gba + GPIO_LVL, vc_lvl);
		io |= (1u << VC_GPIO_SDA) | (1u << VC_GPIO_SCL) |
		    (1u << VC_GPIO_MDIO);
		io &= ~(1u << VC_GPIO_MDC);
		outl(gba + GPIO_IO_SEL, io);
		device_printf(dev, "SoC GPIO @%#x: USE_SEL %#x -> %#x, IO_SEL "
		    "%#x -> %#x\n", gba, use, inl(gba + GPIO_USE_SEL),
		    vc_io_orig, inl(gba + GPIO_IO_SEL));
		if (((inl(gba + GPIO_IO_SEL) ^ io) & VC_GPIO_MASK) != 0) {
			mtx_unlock(&vc_gpio_mtx);
			device_printf(dev, "GPIO direction did not stick\n");
			return (EIO);
		}
		vc_gbase = gba;
	}
	mtx_unlock(&vc_gpio_mtx);
	return (0);
}

/*********************************************************************
 *  Bit-banged clause 22 MDIO on GPIO 13/14 (88E1514 WAN PHYs)
 *********************************************************************/
static void
vc_mdio_send_bit(int bit)
{
	vc_pin_set(VC_GPIO_MDIO, bit);
	DELAY(vc_mdio_hp);
	vc_pin_set(VC_GPIO_MDC, 1);
	DELAY(vc_mdio_hp);
	vc_pin_set(VC_GPIO_MDC, 0);
}

static int
vc_mdio_get_bit(void)
{
	DELAY(vc_mdio_hp);
	vc_pin_set(VC_GPIO_MDC, 1);
	DELAY(vc_mdio_hp);
	vc_pin_set(VC_GPIO_MDC, 0);
	return (vc_pin_get(VC_GPIO_MDIO));
}

static void
vc_mdio_send_num(uint32_t val, int bits)
{
	int i;

	for (i = bits - 1; i >= 0; i--)
		vc_mdio_send_bit((val >> i) & 1);
}

static void
vc_mdio_release(void)
{
	/* drive 0 first so a stale "1" in the level latch can't leak */
	vc_pin_set(VC_GPIO_MDIO, 0);
	vc_pin_dir(VC_GPIO_MDIO, false);
}

static void
vc_mdio_cmd(int op, int phy, int reg)
{
	int i;

	vc_pin_dir(VC_GPIO_MDIO, true);
	for (i = 0; i < 32; i++)		/* preamble */
		vc_mdio_send_bit(1);
	vc_mdio_send_num(0x1, 2);		/* start: 01 */
	vc_mdio_send_num(op, 2);		/* read 10, write 01 */
	vc_mdio_send_num(phy, 5);
	vc_mdio_send_num(reg, 5);
}

static int
vc_gpio_mdio_read(int phy, int reg, u16 *val)
{
	uint16_t v = 0;
	int i;

	mtx_lock(&vc_gpio_mtx);
	vc_mdio_cmd(0x2, phy, reg);
	vc_mdio_release();
	if (vc_mdio_get_bit() != 0) {
		/* PHY did not drive the turnaround low: no PHY here */
		for (i = 0; i < 32; i++)
			vc_mdio_get_bit();
		mtx_unlock(&vc_gpio_mtx);
		return (EIO);
	}
	for (i = 0; i < 16; i++)
		v = (v << 1) | vc_mdio_get_bit();
	vc_mdio_get_bit();
	mtx_unlock(&vc_gpio_mtx);
	*val = v;
	return (0);
}

static int
vc_gpio_mdio_write(int phy, int reg, u16 val)
{
	mtx_lock(&vc_gpio_mtx);
	vc_mdio_cmd(0x1, phy, reg);
	vc_mdio_send_num(0x2, 2);		/* turnaround: 10 */
	vc_mdio_send_num(val, 16);
	vc_mdio_release();
	vc_mdio_get_bit();
	mtx_unlock(&vc_gpio_mtx);
	return (0);
}

/*********************************************************************
 *  Bit-banged I2C on GPIO 11/12 (open drain: release = input)
 *********************************************************************/
#define VC_I2C_DELAY	5

static void
vc_sda(int v)
{
	vc_pin_dir(VC_GPIO_SDA, !v);
	DELAY(VC_I2C_DELAY);
}

static int
vc_scl(int v)
{
	int i;

	vc_pin_dir(VC_GPIO_SCL, !v);
	DELAY(VC_I2C_DELAY);
	if (v) {			/* allow clock stretching */
		for (i = 0; i < 1000 && !vc_pin_get(VC_GPIO_SCL); i++)
			DELAY(1);
		if (!vc_pin_get(VC_GPIO_SCL))
			return (ETIMEDOUT);
	}
	return (0);
}

static void
vc_i2c_start(void)
{
	vc_sda(1);
	vc_scl(1);
	vc_sda(0);
	vc_scl(0);
}

static void
vc_i2c_stop(void)
{
	vc_sda(0);
	vc_scl(1);
	vc_sda(1);
}

static int
vc_i2c_write_byte(uint8_t b)
{
	int i, nack;

	for (i = 7; i >= 0; i--) {
		vc_sda((b >> i) & 1);
		if (vc_scl(1))
			return (ETIMEDOUT);
		vc_scl(0);
	}
	vc_sda(1);
	if (vc_scl(1))
		return (ETIMEDOUT);
	nack = vc_pin_get(VC_GPIO_SDA);
	vc_scl(0);
	return (nack ? EIO : 0);
}

static int
vc_i2c_read_byte(uint8_t *b, bool ack)
{
	uint8_t v = 0;
	int i;

	vc_sda(1);
	for (i = 0; i < 8; i++) {
		if (vc_scl(1))
			return (ETIMEDOUT);
		v = (v << 1) | vc_pin_get(VC_GPIO_SDA);
		vc_scl(0);
	}
	vc_sda(ack ? 0 : 1);
	vc_scl(1);
	vc_scl(0);
	vc_sda(1);
	*b = v;
	return (0);
}

static int
vc_pca9557_read(uint8_t reg, uint8_t *val)
{
	int error;

	mtx_lock(&vc_gpio_mtx);
	vc_i2c_start();
	error = vc_i2c_write_byte(VC_PCA9557_ADDR << 1);
	if (error == 0)
		error = vc_i2c_write_byte(reg);
	if (error == 0) {
		vc_i2c_start();		/* repeated start */
		error = vc_i2c_write_byte((VC_PCA9557_ADDR << 1) | 1);
	}
	if (error == 0)
		error = vc_i2c_read_byte(val, false);
	vc_i2c_stop();
	mtx_unlock(&vc_gpio_mtx);
	return (error);
}

static int
vc_pca9557_write(uint8_t reg, uint8_t val)
{
	int error;

	mtx_lock(&vc_gpio_mtx);
	vc_i2c_start();
	error = vc_i2c_write_byte(VC_PCA9557_ADDR << 1);
	if (error == 0)
		error = vc_i2c_write_byte(reg);
	if (error == 0)
		error = vc_i2c_write_byte(val);
	vc_i2c_stop();
	mtx_unlock(&vc_gpio_mtx);
	return (error);
}

/*
 * Pulse the 88E1514 reset (PCA9557@0x18 pin 4, active low).  Only pin 4
 * is changed: the other pins of this expander drive PCIe/switch resets,
 * RF kill and USB power.
 */
static int
vc_wan_phy_reset(device_t dev)
{
	uint8_t out, cfg;
	int error;

	error = vc_pca9557_read(PCA9557_OUTPUT, &out);
	if (error == 0)
		error = vc_pca9557_read(PCA9557_CONFIG, &cfg);
	if (error != 0) {
		device_printf(dev, "PCA9557@%#x not responding (%d)\n",
		    VC_PCA9557_ADDR, error);
		return (error);
	}
	device_printf(dev, "PCA9557@%#x: output %#x config %#x, "
	    "resetting WAN PHYs\n", VC_PCA9557_ADDR, out, cfg);

	error = vc_pca9557_write(PCA9557_OUTPUT, out & ~VC_PCA9557_PHY_RST);
	if (error == 0)
		error = vc_pca9557_write(PCA9557_CONFIG,
		    cfg & ~VC_PCA9557_PHY_RST);
	if (error != 0)
		return (error);
	pause("vcrst", hz / 10);
	error = vc_pca9557_write(PCA9557_OUTPUT, out | VC_PCA9557_PHY_RST);
	if (error != 0)
		return (error);
	return (0);
}

/*********************************************************************
 *  I354 MDIO master with an explicit address (switch access)
 *********************************************************************/
static u16
vc_swfw_mask(struct e1000_hw *hw)
{
	static const u16 m[] = { E1000_SWFW_PHY0_SM, E1000_SWFW_PHY1_SM,
	    E1000_SWFW_PHY2_SM, E1000_SWFW_PHY3_SM };

	return (m[hw->bus.func & 3]);
}

static int
vc_mdic(struct e1000_hw *hw, int addr, int reg, bool write, u16 *val)
{
	u32 mdic;
	int i;

	if (addr < 0 || addr > 0x1f || reg < 0 || reg > MAX_PHY_REG_ADDRESS)
		return (EINVAL);

	if (hw->mac.ops.acquire_swfw_sync(hw, vc_swfw_mask(hw)))
		return (EBUSY);

	/* 82580-class parts take the PHY address from MDICNFG */
	mdic = E1000_READ_REG(hw, E1000_MDICNFG);
	mdic &= (E1000_MDICNFG_EXT_MDIO | E1000_MDICNFG_COM_MDIO);
	mdic |= (u32)addr << E1000_MDICNFG_PHY_SHIFT;
	E1000_WRITE_REG(hw, E1000_MDICNFG, mdic);

	mdic = ((u32)reg << E1000_MDIC_REG_SHIFT) |
	    ((u32)addr << E1000_MDIC_PHY_SHIFT);
	mdic |= write ? (E1000_MDIC_OP_WRITE | *val) : E1000_MDIC_OP_READ;
	E1000_WRITE_REG(hw, E1000_MDIC, mdic);

	for (i = 0; i < E1000_GEN_POLL_TIMEOUT * 3; i++) {
		DELAY(50);
		mdic = E1000_READ_REG(hw, E1000_MDIC);
		if (mdic & E1000_MDIC_READY)
			break;
	}
	hw->mac.ops.release_swfw_sync(hw, vc_swfw_mask(hw));

	if (!(mdic & E1000_MDIC_READY))
		return (ETIMEDOUT);
	if (mdic & E1000_MDIC_ERROR)
		return (EIO);
	if (!write)
		*val = (u16)mdic;
	return (0);
}

int
e1000_vc_mdio_read(struct e1000_hw *hw, int addr, int reg, u16 *val)
{
	return (vc_mdic(hw, addr, reg, false, val));
}

int
e1000_vc_mdio_write(struct e1000_hw *hw, int addr, int reg, u16 val)
{
	return (vc_mdic(hw, addr, reg, true, &val));
}

/*********************************************************************
 *  88E6176 bring-up as a plain switch
 *********************************************************************/
static int
vc_sw_smi_wait(struct e1000_hw *hw)
{
	u16 v;
	int i, error;

	for (i = 0; i < 100; i++) {
		error = e1000_vc_mdio_read(hw, SW_GLOBAL2, SW_G2_SMI_CMD, &v);
		if (error)
			return (error);
		if (!(v & SW_SMI_BUSY))
			return (0);
		DELAY(10);
	}
	return (ETIMEDOUT);
}

/* Access to a switch-internal PHY (device 0-4) or SerDes (0xf). */
static int
vc_sw_phy_write(struct e1000_hw *hw, int dev, int reg, u16 val)
{
	int error;

	if ((error = vc_sw_smi_wait(hw)) != 0 ||
	    (error = e1000_vc_mdio_write(hw, SW_GLOBAL2, SW_G2_SMI_DATA,
	    val)) != 0 ||
	    (error = e1000_vc_mdio_write(hw, SW_GLOBAL2, SW_G2_SMI_CMD,
	    SW_SMI_C22_WRITE | (dev << 5) | reg)) != 0)
		return (error);
	return (vc_sw_smi_wait(hw));
}

static int
vc_sw_phy_read(struct e1000_hw *hw, int dev, int reg, u16 *val)
{
	int error;

	if ((error = vc_sw_smi_wait(hw)) != 0 ||
	    (error = e1000_vc_mdio_write(hw, SW_GLOBAL2, SW_G2_SMI_CMD,
	    SW_SMI_C22_READ | (dev << 5) | reg)) != 0 ||
	    (error = vc_sw_smi_wait(hw)) != 0)
		return (error);
	return (e1000_vc_mdio_read(hw, SW_GLOBAL2, SW_G2_SMI_DATA, val));
}

static int
vc_switch_up(struct e1000_hw *hw)
{
	device_t dev = vc_dev(hw);
	u16 id, v, st;
	int p, d, error;

	error = e1000_vc_mdio_read(hw, SW_PORT(0), SW_PORT_SWITCH_ID, &id);
	if (error) {
		device_printf(dev, "switch MDIO read failed (%d)\n", error);
		return (error);
	}
	if ((id & 0xfff0) != 0x1760) {
		device_printf(dev, "no 88E6176 on MDIO (id %#06x)\n", id);
		return (ENXIO);
	}

	/* Port 4 SerDes: page 1, autoneg off, power up, soft reset */
	if ((error = vc_sw_phy_write(hw, SW_SERDES_DEV, 22, 1)) != 0 ||
	    (error = vc_sw_phy_read(hw, SW_SERDES_DEV, 0, &v)) != 0 ||
	    (error = vc_sw_phy_write(hw, SW_SERDES_DEV, 0,
	    (v | 0x8100) & ~0x1800)) != 0)
		goto fail;
	pause("vcsw", hz);

	/* Port 4: clear PHYDetect, force MAC 1000/full */
	if ((error = e1000_vc_mdio_read(hw, SW_PORT(4), SW_PORT_STATUS,
	    &st)) != 0 ||
	    (error = e1000_vc_mdio_write(hw, SW_PORT(4), SW_PORT_STATUS,
	    st & ~0x1000)) != 0 ||
	    (error = e1000_vc_mdio_write(hw, SW_PORT(4), SW_PORT_PCS_CTRL,
	    0x000e)) != 0)
		goto fail;

	/* Internal PHYs 0-3: page 0, power up, reset, autoneg 1000/full */
	for (d = 0; d < 4; d++) {
		if ((error = vc_sw_phy_write(hw, d, 22, 0)) != 0 ||
		    (error = vc_sw_phy_write(hw, d, 0, 0x9140)) != 0)
			goto fail;
	}

	/* Ports 0-4 forwarding, 5/6 (RGMII cross-links) disabled */
	for (p = 0; p <= 6; p++) {
		if ((error = e1000_vc_mdio_read(hw, SW_PORT(p), SW_PORT_CTRL,
		    &v)) != 0)
			goto fail;
		v &= ~3;
		if (p <= 4)
			v |= 3;
		if ((error = e1000_vc_mdio_write(hw, SW_PORT(p), SW_PORT_CTRL,
		    v)) != 0)
			goto fail;
	}

	e1000_vc_mdio_read(hw, SW_PORT(4), SW_PORT_STATUS, &st);
	device_printf(dev, "88E6176 (id %#06x) switch %c up, port 4 status "
	    "%#06x\n", id, hw->bus.func == 0 ? 'A' : 'B', st);
	return (0);
fail:
	device_printf(dev, "88E6176 bring-up failed (%d)\n", error);
	return (error);
}

/*
 * One-time WAN MDIO bus diagnostic: GPIO state, idle level of the MDIO
 * line, and a PHY ID scan of all 32 addresses at two bus speeds.
 */
static void
vc_ext_diag(device_t dev)
{
	uint32_t use, io, lvl;
	int idle, a, hp, save, e2, e3, found;
	u16 id2, id3;

	mtx_lock(&vc_gpio_mtx);
	vc_mdio_release();
	DELAY(10);
	idle = vc_pin_get(VC_GPIO_MDIO);
	use = inl(vc_gbase + GPIO_USE_SEL);
	io = inl(vc_gbase + GPIO_IO_SEL);
	lvl = inl(vc_gbase + GPIO_LVL);
	mtx_unlock(&vc_gpio_mtx);
	device_printf(dev, "diag: USE_SEL %#x IO_SEL %#x LVL %#x (shadow "
	    "%#x), MDIO idle level %d\n", use, io, lvl, vc_lvl, idle);

	static const int hps[] = { 1, 20 };
	int i;

	save = vc_mdio_hp;
	for (i = 0; i < nitems(hps); i++) {
		hp = vc_mdio_hp = hps[i];
		found = 0;
		for (a = 0; a < 32; a++) {
			id2 = id3 = 0;
			e2 = vc_gpio_mdio_read(a, 2, &id2);
			e3 = vc_gpio_mdio_read(a, 3, &id3);
			if (e2 == 0 || e3 == 0 || a < 2) {
				device_printf(dev, "diag: hp=%dus addr %d: "
				    "ID %#06x/%#06x (err %d/%d)\n", hp, a,
				    id2, id3, e2, e3);
				found += (e2 == 0 && e3 == 0);
			}
		}
		device_printf(dev, "diag: hp=%dus: %d address(es) answered\n",
		    hp, found);
	}
	vc_mdio_hp = save;
}

static bool
vc_phy_ready(int addr)
{
	u16 id;

	return (vc_gpio_mdio_read(addr, 2, &id) == 0 && id == 0x0141);
}

/*
 * Take both 88E1514s out of reset and wait until both answer on MDIO.
 * Straight out of power-on reset they have been seen not to answer even
 * 500ms after the first reset pulse while a second pulse works, so poll
 * and re-pulse instead of relying on a fixed delay.
 */
static int
vc_wan_bringup(device_t dev)
{
	int attempt, ms;

	for (attempt = 1; attempt <= 3; attempt++) {
		if (vc_wan_phy_reset(dev) != 0)
			return (EIO);
		for (ms = 0; ms <= 2000; ms += 50) {
			if (vc_phy_ready(0) && vc_phy_ready(1)) {
				device_printf(dev, "WAN PHYs answering %d ms "
				    "after reset (attempt %d)\n", ms, attempt);
				return (0);
			}
			pause("vcphy", MAX(1, hz / 20));
		}
		device_printf(dev, "WAN PHYs not answering 2s after reset "
		    "(attempt %d)\n", attempt);
		vc_ext_diag(dev);
	}
	return (ETIMEDOUT);
}

static void
vc_restore(void *arg __unused)
{
	uint8_t out, cfg;

	if (!vc_restore_on_unload || vc_gbase == 0)
		return;
	if (vc_pca9557_read(PCA9557_OUTPUT, &out) == 0 &&
	    vc_pca9557_read(PCA9557_CONFIG, &cfg) == 0) {
		vc_pca9557_write(PCA9557_OUTPUT, out & ~VC_PCA9557_PHY_RST);
		vc_pca9557_write(PCA9557_CONFIG, cfg | VC_PCA9557_PHY_RST);
	}
	mtx_lock(&vc_gpio_mtx);
	/* power-on direction: all four pins inputs */
	outl(vc_gbase + GPIO_IO_SEL, inl(vc_gbase + GPIO_IO_SEL) |
	    VC_GPIO_MASK);
	/* power-on mux on this board: GPIO 13/14 (MDIO/MDC) native */
	outl(vc_gbase + GPIO_USE_SEL, (inl(vc_gbase + GPIO_USE_SEL) &
	    ~VC_GPIO_MASK) | (vc_use_orig & VC_GPIO_MASK &
	    ~((1u << VC_GPIO_MDIO) | (1u << VC_GPIO_MDC))));
	mtx_unlock(&vc_gpio_mtx);
	printf("vcigb: restored power-on WAN PHY reset and GPIO mux\n");
}
SYSUNINIT(vc_restore, SI_SUB_DRIVERS, SI_ORDER_ANY, vc_restore, NULL);

void
e1000_vc_phy_report(struct e1000_hw *hw, s32 ret_val)
{
	if (hw->dev_spec._82575.vc_role != E1000_VC_ROLE_EXTPHY)
		return;
	device_printf(vc_dev(hw), "88E1514 addr %u: id %#010x, "
	    "init_phy_params ret %d\n", hw->phy.addr, hw->phy.id, ret_val);
}

/*********************************************************************
 *  Hooks called from the stock driver
 *********************************************************************/
s32
e1000_vc_probe(struct e1000_hw *hw)
{
	struct e1000_dev_spec_82575 *ds = &hw->dev_spec._82575;
	device_t dev = vc_dev(hw);
	u16 nvm[2] = { 0, 0 };

	ds->vc_role = E1000_VC_ROLE_NONE;
	if (hw->mac.type != e1000_i354)
		return (E1000_SUCCESS);
	if (e1000_read_nvm(hw, VC_NVM_WORD, 2, nvm) != E1000_SUCCESS)
		return (E1000_SUCCESS);
	if (nvm[0] != VC_ID || nvm[1] != VC_ID_EDGE5X0) {
		device_printf(dev, "NVM 0x06/0x07 = %#06x %#06x, not a VeloCloud "
		    "edge5x0 port\n", nvm[0], nvm[1]);
		return (E1000_SUCCESS);
	}

	switch (hw->bus.func) {
	case 0:
	case 1:
		ds->vc_role = E1000_VC_ROLE_SWITCH;
		hw->phy.media_type = e1000_media_type_internal_serdes;
		hw->mac.ops.setup_physical_interface =
		    e1000_vc_setup_serdes_forced;
		device_printf(dev, "VeloCloud edge5x0: switch %c uplink, "
		    "forced 1000/full SerDes\n", hw->bus.func == 0 ? 'A' : 'B');
		return (E1000_SUCCESS);
	case 2:
	case 3:
		if (vc_gpio_setup(dev) != 0)
			return (-E1000_ERR_PHY);
		/* vc_wan_reset_done: 0 = not tried, 1 = up, 2 = failed */
		if (atomic_cmpset_int(&vc_wan_reset_done, 0, 1) &&
		    vc_wan_bringup(dev) != 0)
			atomic_store_rel_int(&vc_wan_reset_done, 2);
		if (vc_wan_reset_done == 2)
			return (-E1000_ERR_PHY);
		ds->vc_role = E1000_VC_ROLE_EXTPHY;
		hw->phy.addr = hw->bus.func - 2;
		device_printf(dev, "VeloCloud edge5x0: GE%u, 88E1514 at GPIO "
		    "MDIO address %u\n", hw->bus.func - 1, hw->phy.addr);
		return (E1000_SUCCESS);
	default:
		return (E1000_SUCCESS);
	}
}

s32
e1000_vc_ext_read_reg(struct e1000_hw *hw, u32 offset, u16 *data)
{
	if (offset > MAX_PHY_REG_ADDRESS)
		return (-E1000_ERR_PARAM);
	if (vc_gpio_mdio_read(hw->phy.addr, offset, data) != 0)
		return (-E1000_ERR_PHY);
	return (E1000_SUCCESS);
}

s32
e1000_vc_ext_write_reg(struct e1000_hw *hw, u32 offset, u16 data)
{
	if (offset > MAX_PHY_REG_ADDRESS)
		return (-E1000_ERR_PARAM);
	if (vc_gpio_mdio_write(hw->phy.addr, offset, data) != 0)
		return (-E1000_ERR_PHY);
	return (E1000_SUCCESS);
}

/*
 * Switch-facing functions: no PHY, so the SGMII autoneg that stock
 * e1000_setup_serdes_link_82575() would run has no partner.  Force the
 * PCS link to 1000/full instead.
 */
s32
e1000_vc_setup_serdes_forced(struct e1000_hw *hw)
{
	u32 reg;

	E1000_WRITE_REG(hw, E1000_SCTL, E1000_SCTL_DISABLE_SERDES_LOOPBACK);

	reg = E1000_READ_REG(hw, E1000_PCS_CFG0);
	reg |= E1000_PCS_CFG_PCS_EN;
	E1000_WRITE_REG(hw, E1000_PCS_CFG0, reg);

	reg = E1000_READ_REG(hw, E1000_CTRL);
	reg |= E1000_CTRL_SLU | E1000_CTRL_SPD_1000 | E1000_CTRL_FD |
	    E1000_CTRL_FRCSPD | E1000_CTRL_FRCDPX;
	E1000_WRITE_REG(hw, E1000_CTRL, reg);

	reg = E1000_READ_REG(hw, E1000_PCS_LCTL);
	reg &= ~(E1000_PCS_LCTL_AN_ENABLE | E1000_PCS_LCTL_AN_RESTART |
	    E1000_PCS_LCTL_AN_TIMEOUT);
	reg |= E1000_PCS_LCTL_FLV_LINK_UP | E1000_PCS_LCTL_FSV_1000 |
	    E1000_PCS_LCTL_FDV_FULL | E1000_PCS_LCTL_FSD |
	    E1000_PCS_LCTL_FORCE_FCTRL | E1000_PCS_LCTL_FORCE_LINK;
	E1000_WRITE_REG(hw, E1000_PCS_LCTL, reg);

	return (E1000_SUCCESS);
}

/*
 * Debug sysctls: dev.vcigb.N.vc_mdio_sel = (addr << 8) | reg selects a
 * register; reading dev.vcigb.N.vc_mdio_val performs the read, writing it
 * performs the write.  Funcs 0/1 use the I354 MDIO (switch), funcs 2/3 the
 * GPIO bus (WAN PHYs).
 */
static int
vc_sysctl_mdio_val(SYSCTL_HANDLER_ARGS)
{
	struct e1000_hw *hw = arg1;
	int sel = vc_dbg_sel[hw->bus.func & 3];
	int addr = (sel >> 8) & 0x1f, reg = sel & 0x1f;
	int error, val;
	u16 v = 0;

	if (hw->dev_spec._82575.vc_role == E1000_VC_ROLE_SWITCH)
		error = e1000_vc_mdio_read(hw, addr, reg, &v);
	else
		error = vc_gpio_mdio_read(addr, reg, &v);
	if (error)
		return (error);
	val = v;
	error = sysctl_handle_int(oidp, &val, 0, req);
	if (error || req->newptr == NULL)
		return (error);
	if (val < 0 || val > 0xffff)
		return (EINVAL);
	if (hw->dev_spec._82575.vc_role == E1000_VC_ROLE_SWITCH)
		return (e1000_vc_mdio_write(hw, addr, reg, val));
	return (vc_gpio_mdio_write(addr, reg, val));
}

void
e1000_vc_attach_post(struct e1000_hw *hw)
{
	device_t dev = vc_dev(hw);
	struct sysctl_ctx_list *ctx;
	struct sysctl_oid_list *child;
	int a;

	switch (hw->dev_spec._82575.vc_role) {
	case E1000_VC_ROLE_SWITCH:
		vc_switch_up(hw);
		break;
	case E1000_VC_ROLE_EXTPHY:
		/*
		 * 88E1514 LEDs (page 3): LED[0] = 1000 link,
		 * LED[1] = 10/100 link + activity.
		 */
		a = hw->phy.addr;
		vc_gpio_mdio_write(a, 22, 3);
		vc_gpio_mdio_write(a, 16, 0x0817);
		vc_gpio_mdio_write(a, 17, 0x0005);
		vc_gpio_mdio_write(a, 22, 0);
		break;
	default:
		return;
	}

	ctx = device_get_sysctl_ctx(dev);
	child = SYSCTL_CHILDREN(device_get_sysctl_tree(dev));
	SYSCTL_ADD_INT(ctx, child, OID_AUTO, "vc_mdio_sel", CTLFLAG_RW,
	    &vc_dbg_sel[hw->bus.func & 3], 0,
	    "VeloCloud debug: MDIO (addr << 8) | reg for vc_mdio_val");
	SYSCTL_ADD_PROC(ctx, child, OID_AUTO, "vc_mdio_val",
	    CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_MPSAFE, hw, 0,
	    vc_sysctl_mdio_val, "I", "VeloCloud debug: MDIO register value");
}
