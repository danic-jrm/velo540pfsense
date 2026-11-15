/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * VeloCloud Edge 520/540 ("edge5x0") board support for the e1000/igb driver.
 * See e1000_vc.c for the board wiring.
 */
#ifndef _E1000_VC_H_
#define _E1000_VC_H_

#define E1000_VC_ROLE_NONE	0
#define E1000_VC_ROLE_SWITCH	1	/* SerDes to 88E6176 port 4, no PHY */
#define E1000_VC_ROLE_EXTPHY	2	/* 88E1514 on the GPIO bit-bang MDIO bus */

/*
 * Called from e1000_init_phy_params_82575() (NVM is already set up).
 * Returns E1000_SUCCESS with dev_spec._82575.vc_role set; NONE means
 * "not a VeloCloud port, continue with stock code".
 */
s32	e1000_vc_probe(struct e1000_hw *hw);
s32	e1000_vc_ext_read_reg(struct e1000_hw *hw, u32 offset, u16 *data);
s32	e1000_vc_ext_write_reg(struct e1000_hw *hw, u32 offset, u16 data);
s32	e1000_vc_setup_serdes_forced(struct e1000_hw *hw);
void	e1000_vc_phy_report(struct e1000_hw *hw, s32 ret_val);

/* Called from em_if_attach_post() after em_reset(). */
void	e1000_vc_attach_post(struct e1000_hw *hw);

/* Raw clause-22 access on this function's own I354 MDIO master. */
int	e1000_vc_mdio_read(struct e1000_hw *hw, int addr, int reg, u16 *val);
int	e1000_vc_mdio_write(struct e1000_hw *hw, int addr, int reg, u16 val);

#endif /* _E1000_VC_H_ */
