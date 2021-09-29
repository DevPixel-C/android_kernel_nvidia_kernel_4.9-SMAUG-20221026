/*
 * Copyright (c) 2014-2017, NVIDIA CORPORATION.  All rights reserved.
 * Copyright (C) 2015 Google, Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */

#include <linux/clk.h>
#include <linux/clk/tegra.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/mailbox_client.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/reset.h>
#include <linux/slab.h>
#include <linux/tegra_prod.h>

#include <soc/tegra/fuse.h>
#include <soc/tegra/pmc.h>

#include "xusb.h"

#define FUSE_SKU_CALIB_HS_CURR_LEVEL_PADX_SHIFT(x) \
					((x) ? (11 + ((x) - 1) * 6) : 0)
#define FUSE_SKU_CALIB_HS_CURR_LEVEL_PAD_MASK 0x3f
#define FUSE_SKU_CALIB_HS_TERM_RANGE_ADJ_SHIFT 7
#define FUSE_SKU_CALIB_HS_TERM_RANGE_ADJ_MASK 0xf

#define FUSE_USB_CALIB_EXT_RPD_CTRL_SHIFT 0
#define FUSE_USB_CALIB_EXT_RPD_CTRL_MASK 0x1f

#define XUSB_PADCTL_USB2_PAD_MUX 0x004
#define XUSB_PADCTL_USB2_PAD_MUX_HSIC_PAD_TRK_SHIFT 16
#define XUSB_PADCTL_USB2_PAD_MUX_HSIC_PAD_TRK_MASK 0x3
#define XUSB_PADCTL_USB2_PAD_MUX_HSIC_PAD_TRK_XUSB 0x1
#define XUSB_PADCTL_USB2_PAD_MUX_USB2_BIAS_PAD_SHIFT 18
#define XUSB_PADCTL_USB2_PAD_MUX_USB2_BIAS_PAD_MASK 0x3
#define XUSB_PADCTL_USB2_PAD_MUX_USB2_BIAS_PAD_XUSB 0x1

#define XUSB_PADCTL_USB2_PORT_CAP 0x008
#define XUSB_PADCTL_USB2_PORT_CAP_PORTX_CAP_DISABLED(x) (0x0 << ((x) * 4))
#define XUSB_PADCTL_USB2_PORT_CAP_PORTX_CAP_HOST(x) (0x1 << ((x) * 4))
#define XUSB_PADCTL_USB2_PORT_CAP_PORTX_CAP_DEVICE(x) (0x2 << ((x) * 4))
#define XUSB_PADCTL_USB2_PORT_CAP_PORTX_CAP_OTG(x) (0x3 << ((x) * 4))
#define XUSB_PADCTL_USB2_PORT_CAP_PORTX_CAP_MASK(x) (0x3 << ((x) * 4))

#define XUSB_PADCTL_SS_PORT_MAP 0x014
#define XUSB_PADCTL_SS_PORT_MAP_PORTX_INTERNAL(x) (1 << (((x) * 5) + 4))
#define XUSB_PADCTL_SS_PORT_MAP_PORTX_MAP_SHIFT(x) ((x) * 5)
#define XUSB_PADCTL_SS_PORT_MAP_PORTX_MAP_MASK(x) (0x7 << ((x) * 5))
#define XUSB_PADCTL_SS_PORT_MAP_PORTX_MAP_DISABLED(x) (0x7 << ((x) * 5))
#define XUSB_PADCTL_SS_PORT_MAP_PORTX_MAP(x, v) (((v) & 0x7) << ((x) * 5))

#define XUSB_PADCTL_ELPG_PROGRAM_0                0x20
#define   USB2_PORT_WAKE_INTERRUPT_ENABLE(x)      BIT((x))
#define   USB2_PORT_WAKEUP_EVENT(x)               BIT((x) + 7)
#define   SS_PORT_WAKE_INTERRUPT_ENABLE(x)        BIT((x) + 14)
#define   SS_PORT_WAKEUP_EVENT(x)                 BIT((x) + 21)
#define   USB2_HSIC_PORT_WAKE_INTERRUPT_ENABLE(x) BIT((x) + 28)
#define   USB2_HSIC_PORT_WAKEUP_EVENT(x)          BIT((x) + 30)
#define   ALL_WAKE_EVENTS ( \
		USB2_PORT_WAKEUP_EVENT(0) | USB2_PORT_WAKEUP_EVENT(1) | \
		USB2_PORT_WAKEUP_EVENT(2) | USB2_PORT_WAKEUP_EVENT(3) | \
		SS_PORT_WAKEUP_EVENT(0) | SS_PORT_WAKEUP_EVENT(1) | \
		SS_PORT_WAKEUP_EVENT(2) | SS_PORT_WAKEUP_EVENT(3) | \
		USB2_HSIC_PORT_WAKEUP_EVENT(0))

#define XUSB_PADCTL_ELPG_PROGRAM_1    0x024
#define   SSPX_ELPG_CLAMP_EN(x)       BIT(0 + (x) * 3)
#define   SSPX_ELPG_CLAMP_EN_EARLY(x) BIT(1 + (x) * 3)
#define   SSPX_ELPG_VCORE_DOWN(x)     BIT(2 + (x) * 3)
#define   AUX_MUX_LP0_CLAMP_EN        BIT(29)
#define   AUX_MUX_LP0_CLAMP_EN_EARLY  BIT(30)
#define   AUX_MUX_LP0_VCORE_DOWN      BIT(31)

#define XUSB_PADCTL_USB3_PAD_MUX 0x028
#define XUSB_PADCTL_USB3_PAD_MUX_PCIE_IDDQ_DISABLE(x) (1 << (1 + (x)))
#define XUSB_PADCTL_USB3_PAD_MUX_SATA_IDDQ_DISABLE(x) (1 << (8 + (x)))

#define XUSB_PADCTL_USB2_BATTERY_CHRG_OTGPADX_CTL0(x) (0x080 + (x) * 0x40)
#define ZIP (1 << 18)
#define ZIN (1 << 22)

#define XUSB_PADCTL_USB2_BATTERY_CHRG_OTGPADX_CTL1(x) (0x084 + (x) * 0x40)
#define XUSB_PADCTL_USB2_BATTERY_CHRG_OTGPAD_CTL1_VREG_LEV_SHIFT 7
#define XUSB_PADCTL_USB2_BATTERY_CHRG_OTGPAD_CTL1_VREG_LEV_MASK 0x3
#define XUSB_PADCTL_USB2_BATTERY_CHRG_OTGPAD_CTL1_VREG_FIX18 (1 << 6)

#define XUSB_PADCTL_USB2_OTG_PADX_CTL0(x) (0x088 + (x) * 0x40)
#define XUSB_PADCTL_USB2_OTG_PAD_CTL0_PD_ZI (1 << 29)
#define XUSB_PADCTL_USB2_OTG_PAD_CTL0_PD2 (1 << 27)
#define XUSB_PADCTL_USB2_OTG_PAD_CTL0_PD (1 << 26)
#define XUSB_PADCTL_USB2_OTG_PAD_CTL0_HS_CURR_LEVEL_SHIFT 0
#define XUSB_PADCTL_USB2_OTG_PAD_CTL0_HS_CURR_LEVEL_MASK 0x3f

#define XUSB_PADCTL_USB2_OTG_PADX_CTL_1(x) (0x8c + (x) * 0x40)
#define   USB2_OTG_PD_DR                   BIT(2)
#define   TERM_RANGE_ADJ(x)                (((x) & 0xf) << 3)
#define   RPD_CTRL(x)                      (((x) & 0x1f) << 26)
#define   RPD_CTRL_VALUE(x)                (((x) << 26) & 0x1f)

#define XUSB_PADCTL_USB2_BIAS_PAD_CTL0 0x284
#define XUSB_PADCTL_USB2_BIAS_PAD_CTL0_PD (1 << 11)
#define XUSB_PADCTL_USB2_BIAS_PAD_CTL0_HS_DISCON_LEVEL_SHIFT 3
#define XUSB_PADCTL_USB2_BIAS_PAD_CTL0_HS_DISCON_LEVEL_MASK 0x7
#define XUSB_PADCTL_USB2_BIAS_PAD_CTL0_HS_DISCON_LEVEL_VAL 0x7
#define XUSB_PADCTL_USB2_BIAS_PAD_CTL0_HS_SQUELCH_LEVEL_SHIFT 0
#define XUSB_PADCTL_USB2_BIAS_PAD_CTL0_HS_SQUELCH_LEVEL_MASK 0x7
#define XUSB_PADCTL_USB2_BIAS_PAD_CTL0_HS_SQUELCH_LEVEL_VAL 0x2

#define XUSB_PADCTL_USB2_BIAS_PAD_CTL_1 (0x288)
#define   TCTRL_VALUE(x)                (((x) & 0x3f) >> 0)
#define   PCTRL_VALUE(x)                (((x) >> 6) & 0x3f)
#define   USB2_TRK_START_TIMER(x)       (((x) & 0x7f) << 12)
#define   USB2_TRK_DONE_RESET_TIMER(x)  (((x) & 0x7f) << 19)
#define   USB2_PD_TRK                   BIT(26)

#define XUSB_PADCTL_HSIC_PADX_CTL0(x) (0x300 + (x) * 0x20)
#define XUSB_PADCTL_HSIC_PAD_CTL0_RPU_STROBE (1 << 18)
#define XUSB_PADCTL_HSIC_PAD_CTL0_RPU_DATA1 (1 << 17)
#define XUSB_PADCTL_HSIC_PAD_CTL0_RPU_DATA0 (1 << 16)
#define XUSB_PADCTL_HSIC_PAD_CTL0_RPD_STROBE (1 << 15)
#define XUSB_PADCTL_HSIC_PAD_CTL0_RPD_DATA1 (1 << 14)
#define XUSB_PADCTL_HSIC_PAD_CTL0_RPD_DATA0 (1 << 13)
#define XUSB_PADCTL_HSIC_PAD_CTL0_PD_ZI_STROBE (1 << 9)
#define XUSB_PADCTL_HSIC_PAD_CTL0_PD_ZI_DATA1 (1 << 8)
#define XUSB_PADCTL_HSIC_PAD_CTL0_PD_ZI_DATA0 (1 << 7)
#define XUSB_PADCTL_HSIC_PAD_CTL0_PD_RX_STROBE (1 << 6)
#define XUSB_PADCTL_HSIC_PAD_CTL0_PD_RX_DATA1 (1 << 5)
#define XUSB_PADCTL_HSIC_PAD_CTL0_PD_RX_DATA0 (1 << 4)
#define XUSB_PADCTL_HSIC_PAD_CTL0_PD_TX_STROBE (1 << 3)
#define XUSB_PADCTL_HSIC_PAD_CTL0_PD_TX_DATA1 (1 << 2)
#define XUSB_PADCTL_HSIC_PAD_CTL0_PD_TX_DATA0 (1 << 1)

#define XUSB_PADCTL_HSIC_PADX_CTL1(x) (0x304 + (x) * 0x20)
#define XUSB_PADCTL_HSIC_PAD_CTL1_TX_RTUNEP_SHIFT 0
#define XUSB_PADCTL_HSIC_PAD_CTL1_TX_RTUNEP_MASK 0xf

#define XUSB_PADCTL_HSIC_PADX_CTL2(x) (0x308 + (x) * 0x20)
#define XUSB_PADCTL_HSIC_PAD_CTL2_RX_STROBE_TRIM_SHIFT 8
#define XUSB_PADCTL_HSIC_PAD_CTL2_RX_STROBE_TRIM_MASK 0xf
#define XUSB_PADCTL_HSIC_PAD_CTL2_RX_DATA_TRIM_SHIFT 0
#define XUSB_PADCTL_HSIC_PAD_CTL2_RX_DATA_TRIM_MASK 0xff

#define XUSB_PADCTL_HSIC_PAD_TRK_CTL 0x340
#define XUSB_PADCTL_HSIC_PAD_TRK_CTL_PD_TRK (1 << 19)
#define XUSB_PADCTL_HSIC_PAD_TRK_CTL_TRK_DONE_RESET_TIMER_SHIFT 12
#define XUSB_PADCTL_HSIC_PAD_TRK_CTL_TRK_DONE_RESET_TIMER_MASK 0x7f
#define XUSB_PADCTL_HSIC_PAD_TRK_CTL_TRK_DONE_RESET_TIMER_VAL 0x0a
#define XUSB_PADCTL_HSIC_PAD_TRK_CTL_TRK_START_TIMER_SHIFT 5
#define XUSB_PADCTL_HSIC_PAD_TRK_CTL_TRK_START_TIMER_MASK 0x7f
#define XUSB_PADCTL_HSIC_PAD_TRK_CTL_TRK_START_TIMER_VAL 0x1e

#define XUSB_PADCTL_HSIC_STRB_TRIM_CONTROL 0x344

#define XUSB_PADCTL_UPHY_PLL_P0_CTL1 0x360
#define XUSB_PADCTL_UPHY_PLL_CTL1_FREQ_NDIV_SHIFT 20
#define XUSB_PADCTL_UPHY_PLL_CTL1_FREQ_NDIV_MASK 0xff
#define XUSB_PADCTL_UPHY_PLL_CTL1_FREQ_NDIV_USB_VAL 0x19
#define XUSB_PADCTL_UPHY_PLL_CTL1_FREQ_NDIV_SATA_VAL 0x1e
#define XUSB_PADCTL_UPHY_PLL_CTL1_FREQ_MDIV_SHIFT 16
#define XUSB_PADCTL_UPHY_PLL_CTL1_FREQ_MDIV_MASK 0x3
#define XUSB_PADCTL_UPHY_PLL_CTL1_LOCKDET_STATUS (1 << 15)
#define XUSB_PADCTL_UPHY_PLL_CTL1_PWR_OVRD (1 << 4)
#define XUSB_PADCTL_UPHY_PLL_CTL1_ENABLE (1 << 3)
#define XUSB_PADCTL_UPHY_PLL_CTL1_SLEEP_SHIFT 1
#define XUSB_PADCTL_UPHY_PLL_CTL1_SLEEP_MASK 0x3
#define XUSB_PADCTL_UPHY_PLL_CTL1_IDDQ (1 << 0)

#define XUSB_PADCTL_UPHY_PLL_P0_CTL2 0x364
#define XUSB_PADCTL_UPHY_PLL_CTL2_CAL_CTRL_SHIFT 4
#define XUSB_PADCTL_UPHY_PLL_CTL2_CAL_CTRL_MASK 0xffffff
#define XUSB_PADCTL_UPHY_PLL_CTL2_CAL_CTRL_VAL 0x136
#define XUSB_PADCTL_UPHY_PLL_CTL2_CAL_OVRD (1 << 2)
#define XUSB_PADCTL_UPHY_PLL_CTL2_CAL_DONE (1 << 1)
#define XUSB_PADCTL_UPHY_PLL_CTL2_CAL_EN (1 << 0)

#define XUSB_PADCTL_UPHY_PLL_P0_CTL4 0x36c
#define XUSB_PADCTL_UPHY_PLL_CTL4_TXCLKREF_EN (1 << 15)
#define XUSB_PADCTL_UPHY_PLL_CTL4_TXCLKREF_SEL_SHIFT 12
#define XUSB_PADCTL_UPHY_PLL_CTL4_TXCLKREF_SEL_MASK 0x3
#define XUSB_PADCTL_UPHY_PLL_CTL4_TXCLKREF_SEL_USB_VAL 0x2
#define XUSB_PADCTL_UPHY_PLL_CTL4_TXCLKREF_SEL_SATA_VAL 0x0
#define XUSB_PADCTL_UPHY_PLL_CTL4_REFCLKBUF_EN (1 << 8)
#define XUSB_PADCTL_UPHY_PLL_CTL4_REFCLK_SEL_SHIFT 4
#define XUSB_PADCTL_UPHY_PLL_CTL4_REFCLK_SEL_MASK 0xf

#define XUSB_PADCTL_UPHY_PLL_P0_CTL5 0x370
#define XUSB_PADCTL_UPHY_PLL_CTL5_DCO_CTRL_SHIFT 16
#define XUSB_PADCTL_UPHY_PLL_CTL5_DCO_CTRL_MASK 0xff
#define XUSB_PADCTL_UPHY_PLL_CTL5_DCO_CTRL_VAL 0x2a

#define XUSB_PADCTL_UPHY_PLL_P0_CTL8 0x37c
#define XUSB_PADCTL_UPHY_PLL_CTL8_RCAL_DONE (1 << 31)
#define XUSB_PADCTL_UPHY_PLL_CTL8_RCAL_OVRD (1 << 15)
#define XUSB_PADCTL_UPHY_PLL_CTL8_RCAL_CLK_EN (1 << 13)
#define XUSB_PADCTL_UPHY_PLL_CTL8_RCAL_EN (1 << 12)

#define XUSB_PADCTL_UPHY_MISC_PAD_PX_CTL1(x) (0x460 + (x) * 0x40)
#define XUSB_PADCTL_UPHY_MISC_PAD_CTL1_AUX_RX_IDLE_MODE_SHIFT 20
#define XUSB_PADCTL_UPHY_MISC_PAD_CTL1_AUX_RX_IDLE_MODE_MASK 0x3
#define XUSB_PADCTL_UPHY_MISC_PAD_CTL1_AUX_RX_IDLE_MODE_VAL 0x1
#define XUSB_PADCTL_UPHY_MISC_PAD_CTL1_AUX_RX_TERM_EN BIT(18)
#define XUSB_PADCTL_UPHY_MISC_PAD_CTL1_AUX_RX_MODE_OVRD BIT(13)

#define XUSB_PADCTL_UPHY_MISC_PAD_PX_CTL8(x) (0x47c + (x) * 0x40)
#define CFG_ADDR(x) (((x) & 0xff) << 16)
#define CFG_WDATA(x) (((x) & 0xffff) << 0)
#define CFG_RESET (1 << 27)
#define CFG_WS (1 << 24)

#define XUSB_PADCTL_UPHY_PLL_S0_CTL1 0x860

#define XUSB_PADCTL_UPHY_PLL_S0_CTL2 0x864

#define XUSB_PADCTL_UPHY_PLL_S0_CTL4 0x86c

#define XUSB_PADCTL_UPHY_PLL_S0_CTL5 0x870

#define XUSB_PADCTL_UPHY_PLL_S0_CTL8 0x87c

#define XUSB_PADCTL_UPHY_PLL_S0_CTL10 0x384

#define XUSB_PADCTL_UPHY_MISC_PAD_S0_CTL1 0x960

#define XUSB_PADCTL_UPHY_USB3_PADX_ECTL1(x) (0xa60 + (x) * 0x40)
#define XUSB_PADCTL_UPHY_USB3_PAD_ECTL1_TX_TERM_CTRL_SHIFT 16
#define XUSB_PADCTL_UPHY_USB3_PAD_ECTL1_TX_TERM_CTRL_MASK 0x3
#define XUSB_PADCTL_UPHY_USB3_PAD_ECTL1_TX_TERM_CTRL_VAL 0x2

#define XUSB_PADCTL_UPHY_USB3_PADX_ECTL2(x) (0xa64 + (x) * 0x40)
#define XUSB_PADCTL_UPHY_USB3_PAD_ECTL2_RX_CTLE_SHIFT 0
#define XUSB_PADCTL_UPHY_USB3_PAD_ECTL2_RX_CTLE_MASK 0xffff
#define XUSB_PADCTL_UPHY_USB3_PAD_ECTL2_RX_CTLE_VAL 0x00fc

#define XUSB_PADCTL_UPHY_USB3_PADX_ECTL3(x) (0xa68 + (x) * 0x40)
#define XUSB_PADCTL_UPHY_USB3_PAD_ECTL3_RX_DFE_VAL 0xc0077f1f

#define XUSB_PADCTL_UPHY_USB3_PADX_ECTL4(x) (0xa6c + (x) * 0x40)
#define XUSB_PADCTL_UPHY_USB3_PAD_ECTL4_RX_CDR_CTRL_SHIFT 16
#define XUSB_PADCTL_UPHY_USB3_PAD_ECTL4_RX_CDR_CTRL_MASK 0xffff
#define XUSB_PADCTL_UPHY_USB3_PAD_ECTL4_RX_CDR_CTRL_VAL 0x01c7

#define XUSB_PADCTL_UPHY_USB3_PADX_ECTL6(x) (0xa74 + (x) * 0x40)
#define XUSB_PADCTL_UPHY_USB3_PAD_ECTL6_RX_EQ_CTRL_H_VAL 0xfcf01368

#define XUSB_PADCTL_USB2_VBUS_ID	(0xc60)
#define		VBUS_OVERRIDE_VBUS_ON	BIT(14)
#define		ID_OVERRIDE(x)			(((x) & 0xf) << 18)
#define		ID_OVERRIDE_GROUNDED	ID_OVERRIDE(0)
#define		ID_OVERRIDE_FLOATING	ID_OVERRIDE(8)

struct init_data {
	u8 cfg_addr;
	u16 cfg_wdata;
};

static struct init_data usb3_pll_g1_init_data[] = {
	{.cfg_addr = 0x2,  .cfg_wdata = 0x0000},
	{.cfg_addr = 0x3,  .cfg_wdata = 0x7051},
	{.cfg_addr = 0x25, .cfg_wdata = 0x0130},
	{.cfg_addr = 0x1E, .cfg_wdata = 0x0017},
};

static struct init_data pcie_lane_data[] = {
	{.cfg_addr = 0x97, .cfg_wdata = 0x0080},
};

static struct init_data usb3_lane_data[] = {
	{.cfg_addr = 0x1,  .cfg_wdata = 0x0002},
	{.cfg_addr = 0x4,  .cfg_wdata = 0x0032},
	{.cfg_addr = 0x7,  .cfg_wdata = 0x0022},
	{.cfg_addr = 0x35, .cfg_wdata = 0x2587},
	{.cfg_addr = 0x49, .cfg_wdata = 0x0FC7},
	{.cfg_addr = 0x52, .cfg_wdata = 0x0001},
	{.cfg_addr = 0x53, .cfg_wdata = 0x3C0F},
	{.cfg_addr = 0x56, .cfg_wdata = 0xC00F},
	{.cfg_addr = 0x5D, .cfg_wdata = 0xFF07},
	{.cfg_addr = 0x5E, .cfg_wdata = 0x141A},
	{.cfg_addr = 0x97, .cfg_wdata = 0x0080},
};

static unsigned int
tegra210_usb3_lane_map(struct tegra_xusb_lane *lane);

struct tegra210_xusb_fuse_calibration {
	u32 hs_curr_level[4];
	u32 hs_term_range_adj;
	u32 rpd_ctrl;
};

struct tegra210_xusb_padctl {
	struct tegra_xusb_padctl base;
	struct tegra210_xusb_fuse_calibration fuse;
	struct tegra_prod *prod_list;
	struct tegra_utmi_pad_config utmi_pad_cfg;
	struct clk *plle;
	struct clk *uphy_mgmt_clk;
	bool sata_used_by_xusb;
};

static inline struct tegra210_xusb_padctl *
to_tegra210_xusb_padctl(struct tegra_xusb_padctl *padctl)
{
	return container_of(padctl, struct tegra210_xusb_padctl, base);
}

static int t210b01_compatible(struct tegra_xusb_padctl *padctl)
{
	struct device_node *np;
	const char *compatible;

	np = padctl->dev->of_node;
	compatible = of_get_property(np, "compatible", NULL);

	if (!compatible) {
		dev_err(padctl->dev, "Failed to get compatible property\n");
		return -ENODEV;
	}

	if (strstr(compatible, "tegra210b01") != NULL)
		return 1;
	return 0;
}

/* must be called under padctl->lock */
static int tegra210_pex_uphy_enable(struct tegra_xusb_padctl *padctl)
{
	struct tegra_xusb_pcie_pad *pcie = to_pcie_pad(padctl->pcie);
	unsigned long timeout;
	u32 value;
	int err, i;

	err = reset_control_deassert(pcie->rst);
	if (err < 0)
		dev_err(padctl->dev, "failed to deassert UPHY PEX PLL reset\n");

	if (t210b01_compatible(padctl) == 1) {
		for (i = 0; i < ARRAY_SIZE(usb3_pll_g1_init_data); i++) {
			value = 0;
			value |= CFG_ADDR(usb3_pll_g1_init_data[i].cfg_addr);
			value |= CFG_WDATA(usb3_pll_g1_init_data[i].cfg_wdata);
			value |= CFG_RESET;
			value |= CFG_WS;
			padctl_writel(padctl, value,
					XUSB_PADCTL_UPHY_PLL_S0_CTL10);
		}
	} else {
		value = padctl_readl(padctl, XUSB_PADCTL_UPHY_PLL_P0_CTL2);
		value &= ~(XUSB_PADCTL_UPHY_PLL_CTL2_CAL_CTRL_MASK <<
			   XUSB_PADCTL_UPHY_PLL_CTL2_CAL_CTRL_SHIFT);
		value |= XUSB_PADCTL_UPHY_PLL_CTL2_CAL_CTRL_VAL <<
			 XUSB_PADCTL_UPHY_PLL_CTL2_CAL_CTRL_SHIFT;
		padctl_writel(padctl, value, XUSB_PADCTL_UPHY_PLL_P0_CTL2);

		value = padctl_readl(padctl, XUSB_PADCTL_UPHY_PLL_P0_CTL5);
		value &= ~(XUSB_PADCTL_UPHY_PLL_CTL5_DCO_CTRL_MASK <<
			   XUSB_PADCTL_UPHY_PLL_CTL5_DCO_CTRL_SHIFT);
		value |= XUSB_PADCTL_UPHY_PLL_CTL5_DCO_CTRL_VAL <<
			 XUSB_PADCTL_UPHY_PLL_CTL5_DCO_CTRL_SHIFT;
		padctl_writel(padctl, value, XUSB_PADCTL_UPHY_PLL_P0_CTL5);
	}

	value = padctl_readl(padctl, XUSB_PADCTL_UPHY_PLL_P0_CTL1);
	value |= XUSB_PADCTL_UPHY_PLL_CTL1_PWR_OVRD;
	padctl_writel(padctl, value, XUSB_PADCTL_UPHY_PLL_P0_CTL1);

	value = padctl_readl(padctl, XUSB_PADCTL_UPHY_PLL_P0_CTL2);
	value |= XUSB_PADCTL_UPHY_PLL_CTL2_CAL_OVRD;
	padctl_writel(padctl, value, XUSB_PADCTL_UPHY_PLL_P0_CTL2);

	value = padctl_readl(padctl, XUSB_PADCTL_UPHY_PLL_P0_CTL8);
	value |= XUSB_PADCTL_UPHY_PLL_CTL8_RCAL_OVRD;
	padctl_writel(padctl, value, XUSB_PADCTL_UPHY_PLL_P0_CTL8);

	value = padctl_readl(padctl, XUSB_PADCTL_UPHY_PLL_P0_CTL4);
	value &= ~((XUSB_PADCTL_UPHY_PLL_CTL4_TXCLKREF_SEL_MASK <<
		    XUSB_PADCTL_UPHY_PLL_CTL4_TXCLKREF_SEL_SHIFT) |
		   (XUSB_PADCTL_UPHY_PLL_CTL4_REFCLK_SEL_MASK <<
		    XUSB_PADCTL_UPHY_PLL_CTL4_REFCLK_SEL_SHIFT));
	value |= (XUSB_PADCTL_UPHY_PLL_CTL4_TXCLKREF_SEL_USB_VAL <<
		  XUSB_PADCTL_UPHY_PLL_CTL4_TXCLKREF_SEL_SHIFT) |
		 XUSB_PADCTL_UPHY_PLL_CTL4_TXCLKREF_EN;
	padctl_writel(padctl, value, XUSB_PADCTL_UPHY_PLL_P0_CTL4);

	value = padctl_readl(padctl, XUSB_PADCTL_UPHY_PLL_P0_CTL1);
	value &= ~((XUSB_PADCTL_UPHY_PLL_CTL1_FREQ_MDIV_MASK <<
		    XUSB_PADCTL_UPHY_PLL_CTL1_FREQ_MDIV_SHIFT) |
		   (XUSB_PADCTL_UPHY_PLL_CTL1_FREQ_NDIV_MASK <<
		    XUSB_PADCTL_UPHY_PLL_CTL1_FREQ_NDIV_SHIFT));
	value |= XUSB_PADCTL_UPHY_PLL_CTL1_FREQ_NDIV_USB_VAL <<
		 XUSB_PADCTL_UPHY_PLL_CTL1_FREQ_NDIV_SHIFT;
	padctl_writel(padctl, value, XUSB_PADCTL_UPHY_PLL_P0_CTL1);

	value = padctl_readl(padctl, XUSB_PADCTL_UPHY_PLL_P0_CTL1);
	value &= ~XUSB_PADCTL_UPHY_PLL_CTL1_IDDQ;
	padctl_writel(padctl, value, XUSB_PADCTL_UPHY_PLL_P0_CTL1);

	value = padctl_readl(padctl, XUSB_PADCTL_UPHY_PLL_P0_CTL1);
	value &= ~(XUSB_PADCTL_UPHY_PLL_CTL1_SLEEP_MASK <<
		   XUSB_PADCTL_UPHY_PLL_CTL1_SLEEP_SHIFT);
	padctl_writel(padctl, value, XUSB_PADCTL_UPHY_PLL_P0_CTL1);

	usleep_range(10, 20);

	value = padctl_readl(padctl, XUSB_PADCTL_UPHY_PLL_P0_CTL4);
	value |= XUSB_PADCTL_UPHY_PLL_CTL4_REFCLKBUF_EN;
	padctl_writel(padctl, value, XUSB_PADCTL_UPHY_PLL_P0_CTL4);

	value = padctl_readl(padctl, XUSB_PADCTL_UPHY_PLL_P0_CTL2);
	value |= XUSB_PADCTL_UPHY_PLL_CTL2_CAL_EN;
	padctl_writel(padctl, value, XUSB_PADCTL_UPHY_PLL_P0_CTL2);

	timeout = jiffies + msecs_to_jiffies(100);

	while (time_before(jiffies, timeout)) {
		value = padctl_readl(padctl, XUSB_PADCTL_UPHY_PLL_P0_CTL2);
		if (value & XUSB_PADCTL_UPHY_PLL_CTL2_CAL_DONE)
			break;

		usleep_range(10, 20);
	}

	if (time_after_eq(jiffies, timeout)) {
		err = -ETIMEDOUT;
	}

	value = padctl_readl(padctl, XUSB_PADCTL_UPHY_PLL_P0_CTL2);
	value &= ~XUSB_PADCTL_UPHY_PLL_CTL2_CAL_EN;
	padctl_writel(padctl, value, XUSB_PADCTL_UPHY_PLL_P0_CTL2);

	timeout = jiffies + msecs_to_jiffies(100);

	while (time_before(jiffies, timeout)) {
		value = padctl_readl(padctl, XUSB_PADCTL_UPHY_PLL_P0_CTL2);
		if (!(value & XUSB_PADCTL_UPHY_PLL_CTL2_CAL_DONE))
			break;

		usleep_range(10, 20);
	}

	if (time_after_eq(jiffies, timeout)) {
		err = -ETIMEDOUT;
	}

	value = padctl_readl(padctl, XUSB_PADCTL_UPHY_PLL_P0_CTL1);
	value |= XUSB_PADCTL_UPHY_PLL_CTL1_ENABLE;
	padctl_writel(padctl, value, XUSB_PADCTL_UPHY_PLL_P0_CTL1);

	timeout = jiffies + msecs_to_jiffies(100);

	while (time_before(jiffies, timeout)) {
		value = padctl_readl(padctl, XUSB_PADCTL_UPHY_PLL_P0_CTL1);
		if (value & XUSB_PADCTL_UPHY_PLL_CTL1_LOCKDET_STATUS)
			break;

		usleep_range(10, 20);
	}

	if (time_after_eq(jiffies, timeout)) {
		err = -ETIMEDOUT;
	}

	value = padctl_readl(padctl, XUSB_PADCTL_UPHY_PLL_P0_CTL8);
	value |= XUSB_PADCTL_UPHY_PLL_CTL8_RCAL_EN |
		 XUSB_PADCTL_UPHY_PLL_CTL8_RCAL_CLK_EN;
	padctl_writel(padctl, value, XUSB_PADCTL_UPHY_PLL_P0_CTL8);

	timeout = jiffies + msecs_to_jiffies(100);

	while (time_before(jiffies, timeout)) {
		value = padctl_readl(padctl, XUSB_PADCTL_UPHY_PLL_P0_CTL8);
		if (value & XUSB_PADCTL_UPHY_PLL_CTL8_RCAL_DONE)
			break;

		usleep_range(10, 20);
	}

	if (time_after_eq(jiffies, timeout)) {
		err = -ETIMEDOUT;
	}

	value = padctl_readl(padctl, XUSB_PADCTL_UPHY_PLL_P0_CTL8);
	value &= ~XUSB_PADCTL_UPHY_PLL_CTL8_RCAL_EN;
	padctl_writel(padctl, value, XUSB_PADCTL_UPHY_PLL_P0_CTL8);

	timeout = jiffies + msecs_to_jiffies(100);

	while (time_before(jiffies, timeout)) {
		value = padctl_readl(padctl, XUSB_PADCTL_UPHY_PLL_P0_CTL8);
		if (!(value & XUSB_PADCTL_UPHY_PLL_CTL8_RCAL_DONE))
			break;

		usleep_range(10, 20);
	}

	if (time_after_eq(jiffies, timeout)) {
		err = -ETIMEDOUT;
	}

	value = padctl_readl(padctl, XUSB_PADCTL_UPHY_PLL_P0_CTL8);
	value &= ~XUSB_PADCTL_UPHY_PLL_CTL8_RCAL_CLK_EN;
	padctl_writel(padctl, value, XUSB_PADCTL_UPHY_PLL_P0_CTL8);

	if (err == -ETIMEDOUT)
		dev_err(padctl->dev, "UPHY PEX PLL calibration timeout\n");

	/* enable PCIE PLL in HW */
	tegra210_xusb_pll_hw_control_enable();

	value = padctl_readl(padctl, XUSB_PADCTL_UPHY_PLL_P0_CTL1);
	value &= ~XUSB_PADCTL_UPHY_PLL_CTL1_PWR_OVRD;
	padctl_writel(padctl, value, XUSB_PADCTL_UPHY_PLL_P0_CTL1);

	value = padctl_readl(padctl, XUSB_PADCTL_UPHY_PLL_P0_CTL2);
	value &= ~XUSB_PADCTL_UPHY_PLL_CTL2_CAL_OVRD;
	padctl_writel(padctl, value, XUSB_PADCTL_UPHY_PLL_P0_CTL2);

	value = padctl_readl(padctl, XUSB_PADCTL_UPHY_PLL_P0_CTL8);
	value &= ~XUSB_PADCTL_UPHY_PLL_CTL8_RCAL_OVRD;
	padctl_writel(padctl, value, XUSB_PADCTL_UPHY_PLL_P0_CTL8);

	usleep_range(10, 20);

	tegra210_xusb_pll_hw_sequence_start();

	return 0;
}

/* must be called under padctl->lock */
static int tegra210_sata_uphy_enable(struct tegra_xusb_padctl *padctl)
{
	struct tegra210_xusb_padctl *priv = to_tegra210_xusb_padctl(padctl);
	struct tegra_xusb_sata_pad *sata = to_sata_pad(padctl->sata);
	unsigned long timeout;
	u32 value;
	int err;

	err = reset_control_deassert(sata->rst);
	if (err < 0)
		dev_err(padctl->dev, "failed to deassert UPHY SATA PLL reset\n");

	value = padctl_readl(padctl, XUSB_PADCTL_UPHY_PLL_S0_CTL2);
	value &= ~(XUSB_PADCTL_UPHY_PLL_CTL2_CAL_CTRL_MASK <<
		   XUSB_PADCTL_UPHY_PLL_CTL2_CAL_CTRL_SHIFT);
	value |= XUSB_PADCTL_UPHY_PLL_CTL2_CAL_CTRL_VAL <<
		 XUSB_PADCTL_UPHY_PLL_CTL2_CAL_CTRL_SHIFT;
	padctl_writel(padctl, value, XUSB_PADCTL_UPHY_PLL_S0_CTL2);

	value = padctl_readl(padctl, XUSB_PADCTL_UPHY_PLL_S0_CTL5);
	value &= ~(XUSB_PADCTL_UPHY_PLL_CTL5_DCO_CTRL_MASK <<
		   XUSB_PADCTL_UPHY_PLL_CTL5_DCO_CTRL_SHIFT);
	value |= XUSB_PADCTL_UPHY_PLL_CTL5_DCO_CTRL_VAL <<
		 XUSB_PADCTL_UPHY_PLL_CTL5_DCO_CTRL_SHIFT;
	padctl_writel(padctl, value, XUSB_PADCTL_UPHY_PLL_S0_CTL5);

	value = padctl_readl(padctl, XUSB_PADCTL_UPHY_PLL_S0_CTL1);
	value |= XUSB_PADCTL_UPHY_PLL_CTL1_PWR_OVRD;
	padctl_writel(padctl, value, XUSB_PADCTL_UPHY_PLL_S0_CTL1);

	value = padctl_readl(padctl, XUSB_PADCTL_UPHY_PLL_S0_CTL2);
	value |= XUSB_PADCTL_UPHY_PLL_CTL2_CAL_OVRD;
	padctl_writel(padctl, value, XUSB_PADCTL_UPHY_PLL_S0_CTL2);

	value = padctl_readl(padctl, XUSB_PADCTL_UPHY_PLL_S0_CTL8);
	value |= XUSB_PADCTL_UPHY_PLL_CTL8_RCAL_OVRD;
	padctl_writel(padctl, value, XUSB_PADCTL_UPHY_PLL_S0_CTL8);

	value = padctl_readl(padctl, XUSB_PADCTL_UPHY_PLL_S0_CTL4);
	value &= ~((XUSB_PADCTL_UPHY_PLL_CTL4_TXCLKREF_SEL_MASK <<
		    XUSB_PADCTL_UPHY_PLL_CTL4_TXCLKREF_SEL_SHIFT) |
		   (XUSB_PADCTL_UPHY_PLL_CTL4_REFCLK_SEL_MASK <<
		    XUSB_PADCTL_UPHY_PLL_CTL4_REFCLK_SEL_SHIFT));
	value |= XUSB_PADCTL_UPHY_PLL_CTL4_TXCLKREF_EN;

	if (priv->sata_used_by_xusb)
		value |= (XUSB_PADCTL_UPHY_PLL_CTL4_TXCLKREF_SEL_USB_VAL <<
			  XUSB_PADCTL_UPHY_PLL_CTL4_TXCLKREF_SEL_SHIFT);
	else
		value |= (XUSB_PADCTL_UPHY_PLL_CTL4_TXCLKREF_SEL_SATA_VAL <<
			  XUSB_PADCTL_UPHY_PLL_CTL4_TXCLKREF_SEL_SHIFT);

	/* XXX PLL0_XDIGCLK_EN */
	/*
	value &= ~(1 << 19);
	padctl_writel(padctl, value, XUSB_PADCTL_UPHY_PLL_S0_CTL4);
	*/

	value = padctl_readl(padctl, XUSB_PADCTL_UPHY_PLL_S0_CTL1);
	value &= ~((XUSB_PADCTL_UPHY_PLL_CTL1_FREQ_MDIV_MASK <<
		    XUSB_PADCTL_UPHY_PLL_CTL1_FREQ_MDIV_SHIFT) |
		   (XUSB_PADCTL_UPHY_PLL_CTL1_FREQ_NDIV_MASK <<
		    XUSB_PADCTL_UPHY_PLL_CTL1_FREQ_NDIV_SHIFT));

	if (priv->sata_used_by_xusb)
		value |= XUSB_PADCTL_UPHY_PLL_CTL1_FREQ_NDIV_USB_VAL <<
			 XUSB_PADCTL_UPHY_PLL_CTL1_FREQ_NDIV_SHIFT;
	else
		value |= XUSB_PADCTL_UPHY_PLL_CTL1_FREQ_NDIV_SATA_VAL <<
			 XUSB_PADCTL_UPHY_PLL_CTL1_FREQ_NDIV_SHIFT;

	padctl_writel(padctl, value, XUSB_PADCTL_UPHY_PLL_S0_CTL1);

	value = padctl_readl(padctl, XUSB_PADCTL_UPHY_PLL_S0_CTL1);
	value &= ~XUSB_PADCTL_UPHY_PLL_CTL1_IDDQ;
	padctl_writel(padctl, value, XUSB_PADCTL_UPHY_PLL_S0_CTL1);

	value = padctl_readl(padctl, XUSB_PADCTL_UPHY_PLL_S0_CTL1);
	value &= ~(XUSB_PADCTL_UPHY_PLL_CTL1_SLEEP_MASK <<
		   XUSB_PADCTL_UPHY_PLL_CTL1_SLEEP_SHIFT);
	padctl_writel(padctl, value, XUSB_PADCTL_UPHY_PLL_S0_CTL1);

	usleep_range(10, 20);

	value = padctl_readl(padctl, XUSB_PADCTL_UPHY_PLL_S0_CTL4);
	value |= XUSB_PADCTL_UPHY_PLL_CTL4_REFCLKBUF_EN;
	padctl_writel(padctl, value, XUSB_PADCTL_UPHY_PLL_S0_CTL4);

	value = padctl_readl(padctl, XUSB_PADCTL_UPHY_PLL_S0_CTL2);
	value |= XUSB_PADCTL_UPHY_PLL_CTL2_CAL_EN;
	padctl_writel(padctl, value, XUSB_PADCTL_UPHY_PLL_S0_CTL2);

	timeout = jiffies + msecs_to_jiffies(100);

	while (time_before(jiffies, timeout)) {
		value = padctl_readl(padctl, XUSB_PADCTL_UPHY_PLL_S0_CTL2);
		if (value & XUSB_PADCTL_UPHY_PLL_CTL2_CAL_DONE)
			break;

		usleep_range(10, 20);
	}

	if (time_after_eq(jiffies, timeout)) {
		err = -ETIMEDOUT;
	}

	value = padctl_readl(padctl, XUSB_PADCTL_UPHY_PLL_S0_CTL2);
	value &= ~XUSB_PADCTL_UPHY_PLL_CTL2_CAL_EN;
	padctl_writel(padctl, value, XUSB_PADCTL_UPHY_PLL_S0_CTL2);

	timeout = jiffies + msecs_to_jiffies(100);

	while (time_before(jiffies, timeout)) {
		value = padctl_readl(padctl, XUSB_PADCTL_UPHY_PLL_S0_CTL2);
		if (!(value & XUSB_PADCTL_UPHY_PLL_CTL2_CAL_DONE))
			break;

		usleep_range(10, 20);
	}

	if (time_after_eq(jiffies, timeout)) {
		err = -ETIMEDOUT;
	}

	value = padctl_readl(padctl, XUSB_PADCTL_UPHY_PLL_S0_CTL1);
	value |= XUSB_PADCTL_UPHY_PLL_CTL1_ENABLE;
	padctl_writel(padctl, value, XUSB_PADCTL_UPHY_PLL_S0_CTL1);

	timeout = jiffies + msecs_to_jiffies(100);

	while (time_before(jiffies, timeout)) {
		value = padctl_readl(padctl, XUSB_PADCTL_UPHY_PLL_S0_CTL1);
		if (value & XUSB_PADCTL_UPHY_PLL_CTL1_LOCKDET_STATUS)
			break;

		usleep_range(10, 20);
	}

	if (time_after_eq(jiffies, timeout)) {
		err = -ETIMEDOUT;
	}

	value = padctl_readl(padctl, XUSB_PADCTL_UPHY_PLL_S0_CTL8);
	value |= XUSB_PADCTL_UPHY_PLL_CTL8_RCAL_EN |
		 XUSB_PADCTL_UPHY_PLL_CTL8_RCAL_CLK_EN;
	padctl_writel(padctl, value, XUSB_PADCTL_UPHY_PLL_S0_CTL8);

	timeout = jiffies + msecs_to_jiffies(100);

	while (time_before(jiffies, timeout)) {
		value = padctl_readl(padctl, XUSB_PADCTL_UPHY_PLL_S0_CTL8);
		if (value & XUSB_PADCTL_UPHY_PLL_CTL8_RCAL_DONE)
			break;

		usleep_range(10, 20);
	}

	if (time_after_eq(jiffies, timeout)) {
		err = -ETIMEDOUT;
	}

	value = padctl_readl(padctl, XUSB_PADCTL_UPHY_PLL_S0_CTL8);
	value &= ~XUSB_PADCTL_UPHY_PLL_CTL8_RCAL_EN;
	padctl_writel(padctl, value, XUSB_PADCTL_UPHY_PLL_S0_CTL8);

	timeout = jiffies + msecs_to_jiffies(100);

	while (time_before(jiffies, timeout)) {
		value = padctl_readl(padctl, XUSB_PADCTL_UPHY_PLL_S0_CTL8);
		if (!(value & XUSB_PADCTL_UPHY_PLL_CTL8_RCAL_DONE))
			break;

		usleep_range(10, 20);
	}

	if (time_after_eq(jiffies, timeout)) {
		err = -ETIMEDOUT;
	}

	value = padctl_readl(padctl, XUSB_PADCTL_UPHY_PLL_S0_CTL8);
	value &= ~XUSB_PADCTL_UPHY_PLL_CTL8_RCAL_CLK_EN;
	padctl_writel(padctl, value, XUSB_PADCTL_UPHY_PLL_S0_CTL8);

	if (err == -ETIMEDOUT)
		dev_err(padctl->dev, "UPHY SATA PLL calibration timeout\n");

	/* enable SATA PLL in HW */
	tegra210_sata_pll_hw_control_enable();

	value = padctl_readl(padctl, XUSB_PADCTL_UPHY_PLL_S0_CTL1);
	value &= ~XUSB_PADCTL_UPHY_PLL_CTL1_PWR_OVRD;
	padctl_writel(padctl, value, XUSB_PADCTL_UPHY_PLL_S0_CTL1);

	value = padctl_readl(padctl, XUSB_PADCTL_UPHY_PLL_S0_CTL2);
	value &= ~XUSB_PADCTL_UPHY_PLL_CTL2_CAL_OVRD;
	padctl_writel(padctl, value, XUSB_PADCTL_UPHY_PLL_S0_CTL2);

	value = padctl_readl(padctl, XUSB_PADCTL_UPHY_PLL_S0_CTL8);
	value &= ~XUSB_PADCTL_UPHY_PLL_CTL8_RCAL_OVRD;
	padctl_writel(padctl, value, XUSB_PADCTL_UPHY_PLL_S0_CTL8);

	usleep_range(10, 20);

	tegra210_sata_pll_hw_sequence_start();

	return 0;
}

static int tegra210_xusb_padctl_enable(struct tegra_xusb_padctl *padctl)
{
	struct tegra210_xusb_padctl *priv = to_tegra210_xusb_padctl(padctl);
	u32 value;
	int err;

	mutex_lock(&padctl->lock);

	if (padctl->enable++ > 0)
		goto out;

	if (tegra210_plle_hw_sequence_is_enabled())
		dev_err(padctl->dev, "PLLE was in HW before init!\n");

	/* enable PLLE in SW */
	err = clk_prepare_enable(priv->plle);
	if (err < 0)
		return err;

	if (t210b01_compatible(padctl) == 1) {
		err = clk_prepare_enable(priv->uphy_mgmt_clk);
		if (err < 0)
			return err;
	}

	/* enable PCIE & SATA PLL in HW */
	tegra210_pex_uphy_enable(padctl);
	if (t210b01_compatible(padctl) == 0)
		tegra210_sata_uphy_enable(padctl);

	/* enable PLLE in HW */
	tegra210_plle_hw_sequence_start();

	value = padctl_readl(padctl, XUSB_PADCTL_ELPG_PROGRAM_1);
	value &= ~AUX_MUX_LP0_CLAMP_EN;
	padctl_writel(padctl, value, XUSB_PADCTL_ELPG_PROGRAM_1);

	usleep_range(100, 200);

	value = padctl_readl(padctl, XUSB_PADCTL_ELPG_PROGRAM_1);
	value &= ~AUX_MUX_LP0_CLAMP_EN_EARLY;
	padctl_writel(padctl, value, XUSB_PADCTL_ELPG_PROGRAM_1);

	usleep_range(100, 200);

	value = padctl_readl(padctl, XUSB_PADCTL_ELPG_PROGRAM_1);
	value &= ~AUX_MUX_LP0_VCORE_DOWN;
	padctl_writel(padctl, value, XUSB_PADCTL_ELPG_PROGRAM_1);

out:
	mutex_unlock(&padctl->lock);
	return 0;
}

static int tegra210_xusb_padctl_disable(struct tegra_xusb_padctl *padctl)
{
	u32 value;

	mutex_lock(&padctl->lock);

	if (WARN_ON(padctl->enable == 0))
		goto out;

	if (--padctl->enable > 0)
		goto out;

	value = padctl_readl(padctl, XUSB_PADCTL_ELPG_PROGRAM_1);
	value |= AUX_MUX_LP0_VCORE_DOWN;
	padctl_writel(padctl, value, XUSB_PADCTL_ELPG_PROGRAM_1);

	usleep_range(100, 200);

	value = padctl_readl(padctl, XUSB_PADCTL_ELPG_PROGRAM_1);
	value |= AUX_MUX_LP0_CLAMP_EN_EARLY;
	padctl_writel(padctl, value, XUSB_PADCTL_ELPG_PROGRAM_1);

	usleep_range(100, 200);

	value = padctl_readl(padctl, XUSB_PADCTL_ELPG_PROGRAM_1);
	value |= AUX_MUX_LP0_CLAMP_EN;
	padctl_writel(padctl, value, XUSB_PADCTL_ELPG_PROGRAM_1);

out:
	mutex_unlock(&padctl->lock);
	return 0;
}

static int tegra210_hsic_set_idle(struct tegra_xusb_padctl *padctl,
				  unsigned int index, bool idle)
{
	u32 value;

	value = padctl_readl(padctl, XUSB_PADCTL_HSIC_PADX_CTL0(index));

	value &= ~(XUSB_PADCTL_HSIC_PAD_CTL0_RPU_DATA0 |
		   XUSB_PADCTL_HSIC_PAD_CTL0_RPU_DATA1 |
		   XUSB_PADCTL_HSIC_PAD_CTL0_RPD_STROBE);

	if (idle)
		value |= XUSB_PADCTL_HSIC_PAD_CTL0_RPD_DATA0 |
			 XUSB_PADCTL_HSIC_PAD_CTL0_RPD_DATA1 |
			 XUSB_PADCTL_HSIC_PAD_CTL0_RPU_STROBE;
	else
		value &= ~(XUSB_PADCTL_HSIC_PAD_CTL0_RPD_DATA0 |
			   XUSB_PADCTL_HSIC_PAD_CTL0_RPD_DATA1 |
			   XUSB_PADCTL_HSIC_PAD_CTL0_RPU_STROBE);

	padctl_writel(padctl, value, XUSB_PADCTL_HSIC_PADX_CTL0(index));

	return 0;
}

static int tegra210_usb3_set_lfps_detect(struct tegra_xusb_padctl *padctl,
					 unsigned int index, bool enable)
{
	struct tegra_xusb_port *port;
	struct tegra_xusb_lane *lane;
	u32 value, offset;

	port = tegra_xusb_find_port(padctl, "usb3", index);
	if (!port)
		return -ENODEV;

	dev_dbg(padctl->dev, "set usb3-%d lfps detect %s\n",
			index, enable ? "enable" : "disable");

	lane = port->lane;

	if (lane->pad == padctl->pcie)
		offset = XUSB_PADCTL_UPHY_MISC_PAD_PX_CTL1(lane->index);
	else
		offset = XUSB_PADCTL_UPHY_MISC_PAD_S0_CTL1;

	value = padctl_readl(padctl, offset);

	value &= ~((XUSB_PADCTL_UPHY_MISC_PAD_CTL1_AUX_RX_IDLE_MODE_MASK <<
		    XUSB_PADCTL_UPHY_MISC_PAD_CTL1_AUX_RX_IDLE_MODE_SHIFT) |
		   XUSB_PADCTL_UPHY_MISC_PAD_CTL1_AUX_RX_TERM_EN |
		   XUSB_PADCTL_UPHY_MISC_PAD_CTL1_AUX_RX_MODE_OVRD);

	if (!enable) {
		value |= (XUSB_PADCTL_UPHY_MISC_PAD_CTL1_AUX_RX_IDLE_MODE_VAL <<
			  XUSB_PADCTL_UPHY_MISC_PAD_CTL1_AUX_RX_IDLE_MODE_SHIFT) |
			 XUSB_PADCTL_UPHY_MISC_PAD_CTL1_AUX_RX_TERM_EN |
			 XUSB_PADCTL_UPHY_MISC_PAD_CTL1_AUX_RX_MODE_OVRD;
	}

	padctl_writel(padctl, value, offset);

	return 0;
}

#define TEGRA210_LANE(_name, _offset, _shift, _mask, _type)		\
	{								\
		.name = _name,						\
		.offset = _offset,					\
		.shift = _shift,					\
		.mask = _mask,						\
		.num_funcs = ARRAY_SIZE(tegra210_##_type##_functions),	\
		.funcs = tegra210_##_type##_functions,			\
	}

static const char *tegra210_usb2_functions[] = {
	"snps",
	"xusb",
	"uart"
};

static const struct tegra_xusb_lane_soc tegra210_usb2_lanes[] = {
	TEGRA210_LANE("usb2-0", 0x004,  0, 0x3, usb2),
	TEGRA210_LANE("usb2-1", 0x004,  2, 0x3, usb2),
	TEGRA210_LANE("usb2-2", 0x004,  4, 0x3, usb2),
	TEGRA210_LANE("usb2-3", 0x004,  6, 0x3, usb2),
};

static struct tegra_xusb_lane *
tegra210_usb2_lane_probe(struct tegra_xusb_pad *pad, struct device_node *np,
			 unsigned int index)
{
	struct tegra_xusb_usb2_lane *usb2;
	int err;

	usb2 = kzalloc(sizeof(*usb2), GFP_KERNEL);
	if (!usb2)
		return ERR_PTR(-ENOMEM);

	INIT_LIST_HEAD(&usb2->base.list);
	usb2->base.soc = &pad->soc->lanes[index];
	usb2->base.index = index;
	usb2->base.pad = pad;
	usb2->base.np = np;

	err = tegra_xusb_lane_parse_dt(&usb2->base, np);
	if (err < 0) {
		kfree(usb2);
		return ERR_PTR(err);
	}

	dev_info(pad->padctl->dev, "dev = %s, lane = %s, function = %s\n",
		dev_name(&pad->lanes[index]->dev), pad->soc->lanes[index].name,
		usb2->base.soc->funcs[usb2->base.function]);

	return &usb2->base;
}

static void tegra210_usb2_lane_remove(struct tegra_xusb_lane *lane)
{
	struct tegra_xusb_usb2_lane *usb2 = to_usb2_lane(lane);

	kfree(usb2);
}

static const struct tegra_xusb_lane_ops tegra210_usb2_lane_ops = {
	.probe = tegra210_usb2_lane_probe,
	.remove = tegra210_usb2_lane_remove,
};

static int tegra210_usb2_phy_init(struct phy *phy)
{
	struct tegra_xusb_lane *lane = phy_get_drvdata(phy);
	struct tegra_xusb_padctl *padctl = lane->pad->padctl;
	unsigned int index = lane->index;
	struct tegra_xusb_usb2_port *port;
	u32 value;
	int err;

	port = tegra_xusb_find_usb2_port(padctl, index);
	if (!port) {
		dev_err(&phy->dev, "no port found for USB2 lane %u\n", index);
		return -ENODEV;
	}

	dev_dbg(padctl->dev, "phy init lane = %s, port = %s\n",
		lane->pad->soc->lanes[lane->index].name,
		dev_name(&port->base.dev));

	mutex_lock(&padctl->lock);

	value = padctl_readl(padctl, XUSB_PADCTL_USB2_PAD_MUX);
	value &= ~(XUSB_PADCTL_USB2_PAD_MUX_USB2_BIAS_PAD_MASK <<
		   XUSB_PADCTL_USB2_PAD_MUX_USB2_BIAS_PAD_SHIFT);
	value |= XUSB_PADCTL_USB2_PAD_MUX_USB2_BIAS_PAD_XUSB <<
		 XUSB_PADCTL_USB2_PAD_MUX_USB2_BIAS_PAD_SHIFT;
	padctl_writel(padctl, value, XUSB_PADCTL_USB2_PAD_MUX);


	/* only enable regulator when OC is disabled for host only ports */
	/* OC is disabled when either oc_pinctrl is NULL or oc_pin is not
	 * defined (-1)
	 */
	if (port->supply && port->port_cap == USB_HOST_CAP &&
		(!padctl->oc_pinctrl || port->oc_pin < 0)) {
		err = regulator_enable(port->supply);
		if (err) {
			mutex_unlock(&padctl->lock);
			return err;
		}
	}

	if (port->port_cap == USB_OTG_CAP) {
		if (padctl->usb2_otg_port_base_1)
			dev_warn(padctl->dev, "enabling OTG on multiple USB2 ports\n");
		padctl->usb2_otg_port_base_1 = index + 1;
		dev_info(padctl->dev, "enabled OTG on UTMI pad %d\n", index);
	}

	mutex_unlock(&padctl->lock);

	return tegra210_xusb_padctl_enable(padctl);
}

static int tegra210_usb2_phy_exit(struct phy *phy)
{
	struct tegra_xusb_lane *lane = phy_get_drvdata(phy);
	struct tegra_xusb_padctl *padctl = lane->pad->padctl;
	unsigned int index = lane->index;
	struct tegra_xusb_usb2_port *port;

	port = tegra_xusb_find_usb2_port(padctl, index);
	if (!port) {
		dev_err(&phy->dev, "no port found for USB2 lane %u\n", index);
		return -ENODEV;
	}

	mutex_lock(&padctl->lock);

	if (port->supply && port->port_cap == USB_HOST_CAP)
		regulator_disable(port->supply);

	if (index == padctl->usb2_otg_port_base_1 - 1)
		padctl->usb2_otg_port_base_1 = 0;

	mutex_unlock(&padctl->lock);

	return tegra210_xusb_padctl_disable(padctl);
}

static int tegra210_usb2_phy_power_on(struct phy *phy)
{
	struct tegra_xusb_lane *lane = phy_get_drvdata(phy);
	struct tegra_xusb_usb2_lane *usb2 = to_usb2_lane(lane);
	struct tegra_xusb_usb2_pad *pad = to_usb2_pad(lane->pad);
	struct tegra_xusb_padctl *padctl = lane->pad->padctl;
	struct tegra210_xusb_padctl *priv;
	struct tegra_xusb_usb2_port *port;
	unsigned int index = lane->index;
	u32 value;
	int err;

	port = tegra_xusb_find_usb2_port(padctl, index);
	if (!port) {
		dev_err(&phy->dev, "no port found for USB2 lane %u\n", index);
		return -ENODEV;
	}

	dev_dbg(padctl->dev, "phy power on lane = %s, port = %s\n",
		lane->pad->soc->lanes[lane->index].name,
		dev_name(&port->base.dev));

	mutex_lock(&padctl->lock);

	priv = to_tegra210_xusb_padctl(padctl);

	if (priv->prod_list) {
		char prod_name[] = "prod_c_utmiX";

		sprintf(prod_name, "prod_c_utmi%d", port->base.index);
		err = tegra_prod_set_by_name(&padctl->regs, prod_name,
							priv->prod_list);
		if (err)
			dev_dbg(&phy->dev,
				"failed to apply prod for utmi pad%d\n",
							port->base.index);

		err = tegra_prod_set_by_name(&padctl->regs, "prod_c_bias",
							priv->prod_list);
		if (err)
			dev_dbg(&phy->dev,
				"failed to apply prod for bias pad\n");
	}

	if (port->usb3_port_fake != -1) {
		value = padctl_readl(padctl, XUSB_PADCTL_SS_PORT_MAP);
		value &= ~XUSB_PADCTL_SS_PORT_MAP_PORTX_MAP_MASK(
					port->usb3_port_fake);
		value |= XUSB_PADCTL_SS_PORT_MAP_PORTX_MAP(
					port->usb3_port_fake, index);
		padctl_writel(padctl, value, XUSB_PADCTL_SS_PORT_MAP);

		value = padctl_readl(padctl, XUSB_PADCTL_ELPG_PROGRAM_1);
		value &= ~SSPX_ELPG_VCORE_DOWN(port->usb3_port_fake);
		padctl_writel(padctl, value, XUSB_PADCTL_ELPG_PROGRAM_1);

		usleep_range(100, 200);

		value = padctl_readl(padctl, XUSB_PADCTL_ELPG_PROGRAM_1);
		value &= ~SSPX_ELPG_CLAMP_EN_EARLY(port->usb3_port_fake);
		padctl_writel(padctl, value, XUSB_PADCTL_ELPG_PROGRAM_1);

		usleep_range(100, 200);

		value = padctl_readl(padctl, XUSB_PADCTL_ELPG_PROGRAM_1);
		value &= ~SSPX_ELPG_CLAMP_EN(port->usb3_port_fake);
		padctl_writel(padctl, value, XUSB_PADCTL_ELPG_PROGRAM_1);
	}

	value = padctl_readl(padctl, XUSB_PADCTL_USB2_BIAS_PAD_CTL0);
	value &= ~((XUSB_PADCTL_USB2_BIAS_PAD_CTL0_HS_SQUELCH_LEVEL_MASK <<
		    XUSB_PADCTL_USB2_BIAS_PAD_CTL0_HS_SQUELCH_LEVEL_SHIFT) |
		   (XUSB_PADCTL_USB2_BIAS_PAD_CTL0_HS_DISCON_LEVEL_MASK <<
		    XUSB_PADCTL_USB2_BIAS_PAD_CTL0_HS_DISCON_LEVEL_SHIFT));
	value |= (XUSB_PADCTL_USB2_BIAS_PAD_CTL0_HS_DISCON_LEVEL_VAL <<
		  XUSB_PADCTL_USB2_BIAS_PAD_CTL0_HS_DISCON_LEVEL_SHIFT);

	if (tegra_sku_info.revision < TEGRA_REVISION_A02)
		value |=
			(XUSB_PADCTL_USB2_BIAS_PAD_CTL0_HS_SQUELCH_LEVEL_VAL <<
			XUSB_PADCTL_USB2_BIAS_PAD_CTL0_HS_SQUELCH_LEVEL_SHIFT);

	padctl_writel(padctl, value, XUSB_PADCTL_USB2_BIAS_PAD_CTL0);

	value = padctl_readl(padctl, XUSB_PADCTL_USB2_PORT_CAP);
	value &= ~XUSB_PADCTL_USB2_PORT_CAP_PORTX_CAP_MASK(index);
	if (port->port_cap == USB_PORT_DISABLED)
		value |= XUSB_PADCTL_USB2_PORT_CAP_PORTX_CAP_DISABLED(index);
	else if (port->port_cap == USB_DEVICE_CAP)
		value |= XUSB_PADCTL_USB2_PORT_CAP_PORTX_CAP_DEVICE(index);
	else if (port->port_cap == USB_HOST_CAP)
		value |= XUSB_PADCTL_USB2_PORT_CAP_PORTX_CAP_HOST(index);
	else if (port->port_cap == USB_OTG_CAP)
		value |= XUSB_PADCTL_USB2_PORT_CAP_PORTX_CAP_OTG(index);
	padctl_writel(padctl, value, XUSB_PADCTL_USB2_PORT_CAP);

	value = padctl_readl(padctl, XUSB_PADCTL_USB2_OTG_PADX_CTL0(index));
	value &= ~((XUSB_PADCTL_USB2_OTG_PAD_CTL0_HS_CURR_LEVEL_MASK <<
		    XUSB_PADCTL_USB2_OTG_PAD_CTL0_HS_CURR_LEVEL_SHIFT) |
		   XUSB_PADCTL_USB2_OTG_PAD_CTL0_PD |
		   XUSB_PADCTL_USB2_OTG_PAD_CTL0_PD2 |
		   XUSB_PADCTL_USB2_OTG_PAD_CTL0_PD_ZI);
	value |= (priv->fuse.hs_curr_level[index] +
		  usb2->hs_curr_level_offset) <<
		 XUSB_PADCTL_USB2_OTG_PAD_CTL0_HS_CURR_LEVEL_SHIFT;
	padctl_writel(padctl, value, XUSB_PADCTL_USB2_OTG_PADX_CTL0(index));

	value = padctl_readl(padctl, XUSB_PADCTL_USB2_OTG_PADX_CTL_1(index));
	value &= ~TERM_RANGE_ADJ(~0);
	value &= ~RPD_CTRL(~0);
	value |= TERM_RANGE_ADJ(priv->fuse.hs_term_range_adj);
	value |= RPD_CTRL(priv->fuse.rpd_ctrl);
	padctl_writel(padctl, value, XUSB_PADCTL_USB2_OTG_PADX_CTL_1(index));

	value = padctl_readl(padctl,
			     XUSB_PADCTL_USB2_BATTERY_CHRG_OTGPADX_CTL1(index));
	value &= ~(XUSB_PADCTL_USB2_BATTERY_CHRG_OTGPAD_CTL1_VREG_LEV_MASK <<
		   XUSB_PADCTL_USB2_BATTERY_CHRG_OTGPAD_CTL1_VREG_LEV_SHIFT);
	value |= XUSB_PADCTL_USB2_BATTERY_CHRG_OTGPAD_CTL1_VREG_FIX18;
	padctl_writel(padctl, value,
		      XUSB_PADCTL_USB2_BATTERY_CHRG_OTGPADX_CTL1(index));

	if (pad->enable > 0) {
		pad->enable++;
		mutex_unlock(&padctl->lock);
		return 0;
	}

	err = clk_prepare_enable(pad->clk);
	if (err)
		goto out;

	value = padctl_readl(padctl, XUSB_PADCTL_USB2_BIAS_PAD_CTL_1);
	value &= ~USB2_TRK_START_TIMER(~0);
	value &= ~USB2_TRK_DONE_RESET_TIMER(~0);
	value |= USB2_TRK_START_TIMER(0x1e);
	value |= USB2_TRK_DONE_RESET_TIMER(0xa);
	padctl_writel(padctl, value, XUSB_PADCTL_USB2_BIAS_PAD_CTL_1);

	value = padctl_readl(padctl, XUSB_PADCTL_USB2_BIAS_PAD_CTL0);
	value &= ~XUSB_PADCTL_USB2_BIAS_PAD_CTL0_PD;
	padctl_writel(padctl, value, XUSB_PADCTL_USB2_BIAS_PAD_CTL0);

	udelay(1);

	value = padctl_readl(padctl, XUSB_PADCTL_USB2_BIAS_PAD_CTL_1);
	value &= ~USB2_PD_TRK;
	padctl_writel(padctl, value, XUSB_PADCTL_USB2_BIAS_PAD_CTL_1);

	udelay(50);

	clk_disable_unprepare(pad->clk);

	pad->enable++;
	mutex_unlock(&padctl->lock);

	return 0;

out:
	mutex_unlock(&padctl->lock);
	return err;
}

static int tegra210_usb2_phy_power_off(struct phy *phy)
{
	struct tegra_xusb_lane *lane = phy_get_drvdata(phy);
	struct tegra_xusb_usb2_pad *pad = to_usb2_pad(lane->pad);
	struct tegra_xusb_padctl *padctl = lane->pad->padctl;
	struct tegra_xusb_usb2_port *port;
	u32 value;

	port = tegra_xusb_find_usb2_port(padctl, lane->index);
	if (!port) {
		dev_err(&phy->dev, "no port found for USB2 lane %u\n",
			lane->index);
		return -ENODEV;
	}

	dev_dbg(padctl->dev, "phy power off lane = %s, port = %s\n",
		lane->pad->soc->lanes[lane->index].name,
		dev_name(&port->base.dev));

	mutex_lock(&padctl->lock);

	if (WARN_ON(pad->enable == 0))
		goto out;

	if (--pad->enable > 0)
		goto out;

	if (port->usb3_port_fake != -1) {
		value = padctl_readl(padctl, XUSB_PADCTL_ELPG_PROGRAM_1);
		value |= SSPX_ELPG_CLAMP_EN_EARLY(port->usb3_port_fake);
		padctl_writel(padctl, value, XUSB_PADCTL_ELPG_PROGRAM_1);

		usleep_range(100, 200);

		value = padctl_readl(padctl, XUSB_PADCTL_ELPG_PROGRAM_1);
		value |= SSPX_ELPG_CLAMP_EN(port->usb3_port_fake);
		padctl_writel(padctl, value, XUSB_PADCTL_ELPG_PROGRAM_1);

		usleep_range(250, 350);

		value = padctl_readl(padctl, XUSB_PADCTL_ELPG_PROGRAM_1);
		value |= SSPX_ELPG_VCORE_DOWN(port->usb3_port_fake);
		padctl_writel(padctl, value, XUSB_PADCTL_ELPG_PROGRAM_1);

		value = padctl_readl(padctl, XUSB_PADCTL_SS_PORT_MAP);
		value &= ~XUSB_PADCTL_SS_PORT_MAP_PORTX_MAP_MASK(
					port->usb3_port_fake);
		value |= XUSB_PADCTL_SS_PORT_MAP_PORTX_MAP_DISABLED(
					port->usb3_port_fake);
		padctl_writel(padctl, value, XUSB_PADCTL_SS_PORT_MAP);
	}

	value = padctl_readl(padctl, XUSB_PADCTL_USB2_BIAS_PAD_CTL0);
	value |= XUSB_PADCTL_USB2_BIAS_PAD_CTL0_PD;
	padctl_writel(padctl, value, XUSB_PADCTL_USB2_BIAS_PAD_CTL0);

out:
	mutex_unlock(&padctl->lock);
	return 0;
}

static int tegra210_utmi_phy_enable_wake(struct phy *phy)
{
	struct tegra_xusb_lane *lane = phy_get_drvdata(phy);
	struct tegra_xusb_padctl *padctl = lane->pad->padctl;
	unsigned int index = lane->index;
	struct device *dev = padctl->dev;
	u32 reg;

	dev_dbg(dev, "phy enable wake on usb2-%d\n", index);

	mutex_lock(&padctl->lock);

	reg = padctl_readl(padctl, XUSB_PADCTL_ELPG_PROGRAM_0);
	reg &= ~ALL_WAKE_EVENTS;
	reg |= USB2_PORT_WAKEUP_EVENT(index);
	padctl_writel(padctl, reg, XUSB_PADCTL_ELPG_PROGRAM_0);

	usleep_range(10, 20);

	reg = padctl_readl(padctl, XUSB_PADCTL_ELPG_PROGRAM_0);
	reg &= ~ALL_WAKE_EVENTS;
	reg |= USB2_PORT_WAKE_INTERRUPT_ENABLE(index);
	padctl_writel(padctl, reg, XUSB_PADCTL_ELPG_PROGRAM_0);

	mutex_unlock(&padctl->lock);

	return 0;
}

static int tegra210_utmi_phy_disable_wake(struct phy *phy)
{
	struct tegra_xusb_lane *lane = phy_get_drvdata(phy);
	struct tegra_xusb_padctl *padctl = lane->pad->padctl;
	unsigned int index = lane->index;
	struct device *dev = padctl->dev;
	u32 reg;

	dev_dbg(dev, "phy disable wake on usb2-%d\n", index);

	mutex_lock(&padctl->lock);

	reg = padctl_readl(padctl, XUSB_PADCTL_ELPG_PROGRAM_0);
	reg &= ~ALL_WAKE_EVENTS;
	reg &= ~USB2_PORT_WAKE_INTERRUPT_ENABLE(index);
	padctl_writel(padctl, reg, XUSB_PADCTL_ELPG_PROGRAM_0);

	usleep_range(10, 20);

	reg = padctl_readl(padctl, XUSB_PADCTL_ELPG_PROGRAM_0);
	reg &= ~ALL_WAKE_EVENTS;
	reg |= USB2_PORT_WAKEUP_EVENT(index);
	padctl_writel(padctl, reg, XUSB_PADCTL_ELPG_PROGRAM_0);

	mutex_unlock(&padctl->lock);

	return 0;
}

static void tegra210_utmi_phy_get_pad_config(
				struct tegra_xusb_padctl *padctl,
				int port, struct tegra_utmi_pad_config *config)
{
	u32 reg;

	reg = padctl_readl(padctl, XUSB_PADCTL_USB2_BIAS_PAD_CTL_1);
	config->tctrl = TCTRL_VALUE(reg);
	config->pctrl = PCTRL_VALUE(reg);

	reg = padctl_readl(padctl, XUSB_PADCTL_USB2_OTG_PADX_CTL_1(port));
	config->rpd_ctrl = RPD_CTRL_VALUE(reg);
}

static const struct phy_ops tegra210_usb2_phy_ops = {
	.init = tegra210_usb2_phy_init,
	.exit = tegra210_usb2_phy_exit,
	.power_on = tegra210_usb2_phy_power_on,
	.power_off = tegra210_usb2_phy_power_off,
	.owner = THIS_MODULE,
};

static inline bool is_utmi_phy(struct phy *phy)
{
	return phy->ops == &tegra210_usb2_phy_ops;
}

static bool is_utmi_phy_has_otg_cap(struct tegra_xusb_padctl *padctl,
				struct phy *phy)
{
	struct tegra_xusb_lane *lane;
	unsigned int index;
	struct tegra_xusb_usb2_port *port;

	if (!phy)
		return false;

	lane = phy_get_drvdata(phy);
	index = lane->index;

	port = tegra_xusb_find_usb2_port(padctl, index);
	if (!port) {
		dev_err(padctl->dev, "no port found for USB2 lane %u\n", index);
		return -ENODEV;
	}

	return port->port_cap == USB_OTG_CAP;
}

static struct tegra_xusb_pad *
tegra210_usb2_pad_probe(struct tegra_xusb_padctl *padctl,
			const struct tegra_xusb_pad_soc *soc,
			struct device_node *np)
{
	struct tegra_xusb_usb2_pad *usb2;
	struct tegra_xusb_pad *pad;
	int err;

	usb2 = kzalloc(sizeof(*usb2), GFP_KERNEL);
	if (!usb2)
		return ERR_PTR(-ENOMEM);

	pad = &usb2->base;
	pad->ops = &tegra210_usb2_lane_ops;
	pad->soc = soc;

	err = tegra_xusb_pad_init(pad, padctl, np);
	if (err < 0) {
		kfree(usb2);
		goto out;
	}

	usb2->clk = devm_clk_get(&pad->dev, "trk");
	if (IS_ERR(usb2->clk)) {
		err = PTR_ERR(usb2->clk);
		dev_err(&pad->dev, "failed to get trk clock: %d\n", err);
		goto unregister;
	}

	err = tegra_xusb_pad_register(pad, &tegra210_usb2_phy_ops);
	if (err < 0)
		goto unregister;

	dev_set_drvdata(&pad->dev, pad);

	return pad;

unregister:
	device_unregister(&pad->dev);
out:
	return ERR_PTR(err);
}

static void tegra210_usb2_pad_remove(struct tegra_xusb_pad *pad)
{
	struct tegra_xusb_usb2_pad *usb2 = to_usb2_pad(pad);

	kfree(usb2);
}

static const struct tegra_xusb_pad_ops tegra210_usb2_ops = {
	.probe = tegra210_usb2_pad_probe,
	.remove = tegra210_usb2_pad_remove,
};

static const struct tegra_xusb_pad_soc tegra210_usb2_pad = {
	.name = "usb2",
	.num_lanes = ARRAY_SIZE(tegra210_usb2_lanes),
	.lanes = tegra210_usb2_lanes,
	.ops = &tegra210_usb2_ops,
};

static const char *tegra210_hsic_functions[] = {
	"snps",
	"xusb",
};

static const struct tegra_xusb_lane_soc tegra210_hsic_lanes[] = {
	TEGRA210_LANE("hsic-0", 0x004, 14, 0x1, hsic),
};

static struct tegra_xusb_lane *
tegra210_hsic_lane_probe(struct tegra_xusb_pad *pad, struct device_node *np,
			 unsigned int index)
{
	struct tegra_xusb_hsic_lane *hsic;
	int err;

	hsic = kzalloc(sizeof(*hsic), GFP_KERNEL);
	if (!hsic)
		return ERR_PTR(-ENOMEM);

	INIT_LIST_HEAD(&hsic->base.list);
	hsic->base.soc = &pad->soc->lanes[index];
	hsic->base.index = index;
	hsic->base.pad = pad;
	hsic->base.np = np;

	err = tegra_xusb_lane_parse_dt(&hsic->base, np);
	if (err < 0) {
		kfree(hsic);
		return ERR_PTR(err);
	}

	dev_info(pad->padctl->dev, "dev = %s, lane = %s, function = %s\n",
		dev_name(&pad->lanes[index]->dev), pad->soc->lanes[index].name,
		hsic->base.soc->funcs[hsic->base.function]);

	return &hsic->base;
}

static void tegra210_hsic_lane_remove(struct tegra_xusb_lane *lane)
{
	struct tegra_xusb_hsic_lane *hsic = to_hsic_lane(lane);

	kfree(hsic);
}

static const struct tegra_xusb_lane_ops tegra210_hsic_lane_ops = {
	.probe = tegra210_hsic_lane_probe,
	.remove = tegra210_hsic_lane_remove,
};

static int tegra210_hsic_phy_init(struct phy *phy)
{
	struct tegra_xusb_lane *lane = phy_get_drvdata(phy);
	struct tegra_xusb_padctl *padctl = lane->pad->padctl;
	u32 value;

	dev_dbg(padctl->dev, "phy init lane = %s\n",
		lane->pad->soc->lanes[lane->index].name);

	mutex_lock(&padctl->lock);

	value = padctl_readl(padctl, XUSB_PADCTL_USB2_PAD_MUX);
	value &= ~(XUSB_PADCTL_USB2_PAD_MUX_HSIC_PAD_TRK_MASK <<
		   XUSB_PADCTL_USB2_PAD_MUX_HSIC_PAD_TRK_SHIFT);
	value |= XUSB_PADCTL_USB2_PAD_MUX_HSIC_PAD_TRK_XUSB <<
		 XUSB_PADCTL_USB2_PAD_MUX_HSIC_PAD_TRK_SHIFT;
	padctl_writel(padctl, value, XUSB_PADCTL_USB2_PAD_MUX);

	mutex_unlock(&padctl->lock);

	return tegra210_xusb_padctl_enable(padctl);
}

static int tegra210_hsic_phy_exit(struct phy *phy)
{
	struct tegra_xusb_lane *lane = phy_get_drvdata(phy);

	return tegra210_xusb_padctl_disable(lane->pad->padctl);
}

static int tegra210_hsic_phy_power_on(struct phy *phy)
{
	struct tegra_xusb_lane *lane = phy_get_drvdata(phy);
	struct tegra_xusb_hsic_lane *hsic = to_hsic_lane(lane);
	struct tegra_xusb_hsic_pad *pad = to_hsic_pad(lane->pad);
	struct tegra_xusb_padctl *padctl = lane->pad->padctl;
	struct tegra210_xusb_padctl *priv;
	unsigned int index = lane->index;
	u32 value;
	int err;

	dev_dbg(padctl->dev, "phy power on lane = %s\n",
		lane->pad->soc->lanes[lane->index].name);

	mutex_lock(&padctl->lock);

	priv = to_tegra210_xusb_padctl(padctl);

	if (priv->prod_list) {
		char prod_name[] = "prod_c_hsicX";

		sprintf(prod_name, "prod_c_hsic%d", 0);
		err = tegra_prod_set_by_name(&padctl->regs, prod_name,
							priv->prod_list);
		if (err)
			dev_dbg(&phy->dev,
			"failed to apply prod for hsic pad%d\n", 0);
	}

	err = regulator_enable(pad->supply);
	if (err) {
		mutex_unlock(&padctl->lock);
		return err;
	}

	padctl_writel(padctl, hsic->strobe_trim,
		      XUSB_PADCTL_HSIC_STRB_TRIM_CONTROL);

	value = padctl_readl(padctl, XUSB_PADCTL_HSIC_PADX_CTL1(index));
	value &= ~(XUSB_PADCTL_HSIC_PAD_CTL1_TX_RTUNEP_MASK <<
		   XUSB_PADCTL_HSIC_PAD_CTL1_TX_RTUNEP_SHIFT);
	value |= (hsic->tx_rtune_p <<
		  XUSB_PADCTL_HSIC_PAD_CTL1_TX_RTUNEP_SHIFT);
	padctl_writel(padctl, value, XUSB_PADCTL_HSIC_PADX_CTL1(index));

	value = padctl_readl(padctl, XUSB_PADCTL_HSIC_PADX_CTL2(index));
	value &= ~((XUSB_PADCTL_HSIC_PAD_CTL2_RX_STROBE_TRIM_MASK <<
		    XUSB_PADCTL_HSIC_PAD_CTL2_RX_STROBE_TRIM_SHIFT) |
		   (XUSB_PADCTL_HSIC_PAD_CTL2_RX_DATA_TRIM_MASK <<
		    XUSB_PADCTL_HSIC_PAD_CTL2_RX_DATA_TRIM_SHIFT));
	value |= (hsic->rx_strobe_trim <<
		  XUSB_PADCTL_HSIC_PAD_CTL2_RX_STROBE_TRIM_SHIFT) |
		 (hsic->rx_data_trim <<
		  XUSB_PADCTL_HSIC_PAD_CTL2_RX_DATA_TRIM_SHIFT);
	padctl_writel(padctl, value, XUSB_PADCTL_HSIC_PADX_CTL2(index));

	value = padctl_readl(padctl, XUSB_PADCTL_HSIC_PADX_CTL0(index));
	value &= ~(XUSB_PADCTL_HSIC_PAD_CTL0_RPU_DATA0 |
		   XUSB_PADCTL_HSIC_PAD_CTL0_RPU_DATA1 |
		   XUSB_PADCTL_HSIC_PAD_CTL0_RPU_STROBE |
		   XUSB_PADCTL_HSIC_PAD_CTL0_PD_RX_DATA0 |
		   XUSB_PADCTL_HSIC_PAD_CTL0_PD_RX_DATA1 |
		   XUSB_PADCTL_HSIC_PAD_CTL0_PD_RX_STROBE |
		   XUSB_PADCTL_HSIC_PAD_CTL0_PD_ZI_DATA0 |
		   XUSB_PADCTL_HSIC_PAD_CTL0_PD_ZI_DATA1 |
		   XUSB_PADCTL_HSIC_PAD_CTL0_PD_ZI_STROBE |
		   XUSB_PADCTL_HSIC_PAD_CTL0_PD_TX_DATA0 |
		   XUSB_PADCTL_HSIC_PAD_CTL0_PD_TX_DATA1 |
		   XUSB_PADCTL_HSIC_PAD_CTL0_PD_TX_STROBE);
	value |= XUSB_PADCTL_HSIC_PAD_CTL0_RPD_DATA0 |
		 XUSB_PADCTL_HSIC_PAD_CTL0_RPD_DATA1 |
		 XUSB_PADCTL_HSIC_PAD_CTL0_RPD_STROBE;
	padctl_writel(padctl, value, XUSB_PADCTL_HSIC_PADX_CTL0(index));

	err = clk_prepare_enable(pad->clk);
	if (err)
		goto disable;

	value = padctl_readl(padctl, XUSB_PADCTL_HSIC_PAD_TRK_CTL);
	value &= ~((XUSB_PADCTL_HSIC_PAD_TRK_CTL_TRK_START_TIMER_MASK <<
		    XUSB_PADCTL_HSIC_PAD_TRK_CTL_TRK_START_TIMER_SHIFT) |
		   (XUSB_PADCTL_HSIC_PAD_TRK_CTL_TRK_DONE_RESET_TIMER_MASK <<
		    XUSB_PADCTL_HSIC_PAD_TRK_CTL_TRK_DONE_RESET_TIMER_SHIFT));
	value |= (XUSB_PADCTL_HSIC_PAD_TRK_CTL_TRK_START_TIMER_VAL <<
		  XUSB_PADCTL_HSIC_PAD_TRK_CTL_TRK_START_TIMER_SHIFT) |
		 (XUSB_PADCTL_HSIC_PAD_TRK_CTL_TRK_DONE_RESET_TIMER_VAL <<
		  XUSB_PADCTL_HSIC_PAD_TRK_CTL_TRK_DONE_RESET_TIMER_SHIFT);
	padctl_writel(padctl, value, XUSB_PADCTL_HSIC_PAD_TRK_CTL);

	udelay(1);

	value = padctl_readl(padctl, XUSB_PADCTL_HSIC_PAD_TRK_CTL);
	value &= ~XUSB_PADCTL_HSIC_PAD_TRK_CTL_PD_TRK;
	padctl_writel(padctl, value, XUSB_PADCTL_HSIC_PAD_TRK_CTL);

	udelay(50);

	clk_disable_unprepare(pad->clk);

	mutex_unlock(&padctl->lock);
	return 0;

disable:
	regulator_disable(pad->supply);
	mutex_unlock(&padctl->lock);
	return err;
}

static int tegra210_hsic_phy_power_off(struct phy *phy)
{
	struct tegra_xusb_lane *lane = phy_get_drvdata(phy);
	struct tegra_xusb_hsic_pad *pad = to_hsic_pad(lane->pad);
	struct tegra_xusb_padctl *padctl = lane->pad->padctl;
	unsigned int index = lane->index;
	u32 value;

	dev_dbg(padctl->dev, "phy power off lane = %s\n",
		lane->pad->soc->lanes[lane->index].name);

	mutex_lock(&padctl->lock);

	value = padctl_readl(padctl, XUSB_PADCTL_HSIC_PADX_CTL0(index));
	value |= XUSB_PADCTL_HSIC_PAD_CTL0_PD_RX_DATA0 |
		 XUSB_PADCTL_HSIC_PAD_CTL0_PD_RX_DATA1 |
		 XUSB_PADCTL_HSIC_PAD_CTL0_PD_RX_STROBE |
		 XUSB_PADCTL_HSIC_PAD_CTL0_PD_ZI_DATA0 |
		 XUSB_PADCTL_HSIC_PAD_CTL0_PD_ZI_DATA1 |
		 XUSB_PADCTL_HSIC_PAD_CTL0_PD_ZI_STROBE |
		 XUSB_PADCTL_HSIC_PAD_CTL0_PD_TX_DATA0 |
		 XUSB_PADCTL_HSIC_PAD_CTL0_PD_TX_DATA1 |
		 XUSB_PADCTL_HSIC_PAD_CTL0_PD_TX_STROBE;
	padctl_writel(padctl, value, XUSB_PADCTL_HSIC_PADX_CTL1(index));

	regulator_disable(pad->supply);

	mutex_unlock(&padctl->lock);
	return 0;
}

static int tegra210_hsic_phy_enable_wake(struct phy *phy)
{
	struct tegra_xusb_lane *lane = phy_get_drvdata(phy);
	struct tegra_xusb_padctl *padctl = lane->pad->padctl;
	unsigned int index = lane->index;
	struct device *dev = padctl->dev;
	u32 reg;

	dev_dbg(dev, "phy enable wake on hsic-%d\n", index);

	mutex_lock(&padctl->lock);

	reg = padctl_readl(padctl, XUSB_PADCTL_ELPG_PROGRAM_0);
	reg &= ~ALL_WAKE_EVENTS;
	reg |= USB2_HSIC_PORT_WAKEUP_EVENT(index);
	padctl_writel(padctl, reg, XUSB_PADCTL_ELPG_PROGRAM_0);

	usleep_range(10, 20);

	reg = padctl_readl(padctl, XUSB_PADCTL_ELPG_PROGRAM_0);
	reg &= ~ALL_WAKE_EVENTS;
	reg |= USB2_HSIC_PORT_WAKE_INTERRUPT_ENABLE(index);
	padctl_writel(padctl, reg, XUSB_PADCTL_ELPG_PROGRAM_0);

	mutex_unlock(&padctl->lock);

	return 0;
}

static int tegra210_hsic_phy_disable_wake(struct phy *phy)
{
	struct tegra_xusb_lane *lane = phy_get_drvdata(phy);
	struct tegra_xusb_padctl *padctl = lane->pad->padctl;
	unsigned int index = lane->index;
	struct device *dev = padctl->dev;
	u32 reg;

	dev_dbg(dev, "phy disable wake on hsic-%d\n", index);

	mutex_lock(&padctl->lock);

	reg = padctl_readl(padctl, XUSB_PADCTL_ELPG_PROGRAM_0);
	reg &= ~ALL_WAKE_EVENTS;
	reg &= ~~USB2_HSIC_PORT_WAKE_INTERRUPT_ENABLE(index);
	padctl_writel(padctl, reg, XUSB_PADCTL_ELPG_PROGRAM_0);

	usleep_range(10, 20);

	reg = padctl_readl(padctl, XUSB_PADCTL_ELPG_PROGRAM_0);
	reg &= ~ALL_WAKE_EVENTS;
	reg |= USB2_HSIC_PORT_WAKEUP_EVENT(index);
	padctl_writel(padctl, reg, XUSB_PADCTL_ELPG_PROGRAM_0);

	mutex_unlock(&padctl->lock);

	return 0;
}

static const struct phy_ops tegra210_hsic_phy_ops = {
	.init = tegra210_hsic_phy_init,
	.exit = tegra210_hsic_phy_exit,
	.power_on = tegra210_hsic_phy_power_on,
	.power_off = tegra210_hsic_phy_power_off,
	.owner = THIS_MODULE,
};

static inline bool is_hsic_phy(struct phy *phy)
{
	return phy->ops == &tegra210_hsic_phy_ops;
}

static struct tegra_xusb_pad *
tegra210_hsic_pad_probe(struct tegra_xusb_padctl *padctl,
			const struct tegra_xusb_pad_soc *soc,
			struct device_node *np)
{
	struct tegra_xusb_hsic_pad *hsic;
	struct tegra_xusb_pad *pad;
	int err;

	hsic = kzalloc(sizeof(*hsic), GFP_KERNEL);
	if (!hsic)
		return ERR_PTR(-ENOMEM);

	pad = &hsic->base;
	pad->ops = &tegra210_hsic_lane_ops;
	pad->soc = soc;

	err = tegra_xusb_pad_init(pad, padctl, np);
	if (err < 0) {
		kfree(hsic);
		goto out;
	}

	hsic->clk = devm_clk_get(&pad->dev, "trk");
	if (IS_ERR(hsic->clk)) {
		err = PTR_ERR(hsic->clk);
		dev_err(&pad->dev, "failed to get trk clock: %d\n", err);
		goto unregister;
	}

	err = tegra_xusb_pad_register(pad, &tegra210_hsic_phy_ops);
	if (err < 0)
		goto unregister;

	dev_set_drvdata(&pad->dev, pad);

	return pad;

unregister:
	device_unregister(&pad->dev);
out:
	return ERR_PTR(err);
}

static void tegra210_hsic_pad_remove(struct tegra_xusb_pad *pad)
{
	struct tegra_xusb_hsic_pad *hsic = to_hsic_pad(pad);

	kfree(hsic);
}

static const struct tegra_xusb_pad_ops tegra210_hsic_ops = {
	.probe = tegra210_hsic_pad_probe,
	.remove = tegra210_hsic_pad_remove,
};

static const struct tegra_xusb_pad_soc tegra210_hsic_pad = {
	.name = "hsic",
	.num_lanes = ARRAY_SIZE(tegra210_hsic_lanes),
	.lanes = tegra210_hsic_lanes,
	.ops = &tegra210_hsic_ops,
};

static const char *tegra210_pcie_functions[] = {
	"pcie-x1",
	"xusb",
	"sata",
	"pcie-x4",
};

static const struct tegra_xusb_lane_soc tegra210_pcie_lanes[] = {
	TEGRA210_LANE("pcie-0", 0x028, 12, 0x3, pcie),
	TEGRA210_LANE("pcie-1", 0x028, 14, 0x3, pcie),
	TEGRA210_LANE("pcie-2", 0x028, 16, 0x3, pcie),
	TEGRA210_LANE("pcie-3", 0x028, 18, 0x3, pcie),
	TEGRA210_LANE("pcie-4", 0x028, 20, 0x3, pcie),
	TEGRA210_LANE("pcie-5", 0x028, 22, 0x3, pcie),
	TEGRA210_LANE("pcie-6", 0x028, 24, 0x3, pcie),
};

static const struct tegra_xusb_lane_soc tegra210b01_pcie_lanes[] = {
	TEGRA210_LANE("pcie-0", 0x28, 12, 0x3, pcie),
	TEGRA210_LANE("pcie-1", 0x28, 14, 0x3, pcie),
	TEGRA210_LANE("pcie-2", 0x28, 16, 0x3, pcie),
	TEGRA210_LANE("pcie-3", 0x28, 18, 0x3, pcie),
	TEGRA210_LANE("pcie-4", 0x28, 20, 0x3, pcie),
	TEGRA210_LANE("pcie-5", 0x28, 22, 0x3, pcie),
};

static struct tegra_xusb_lane *
tegra210_pcie_lane_probe(struct tegra_xusb_pad *pad, struct device_node *np,
			 unsigned int index)
{
	struct tegra_xusb_pcie_lane *pcie;
	int err;

	pcie = kzalloc(sizeof(*pcie), GFP_KERNEL);
	if (!pcie)
		return ERR_PTR(-ENOMEM);

	INIT_LIST_HEAD(&pcie->base.list);
	pcie->base.soc = &pad->soc->lanes[index];
	pcie->base.index = index;
	pcie->base.pad = pad;
	pcie->base.np = np;

	err = tegra_xusb_lane_parse_dt(&pcie->base, np);
	if (err < 0) {
		kfree(pcie);
		return ERR_PTR(err);
	}

	dev_info(pad->padctl->dev, "dev = %s, lane = %s, function = %s\n",
		dev_name(&pad->lanes[index]->dev), pad->soc->lanes[index].name,
		pcie->base.soc->funcs[pcie->base.function]);

	return &pcie->base;
}

static void tegra210_pcie_lane_remove(struct tegra_xusb_lane *lane)
{
	struct tegra_xusb_pcie_lane *pcie = to_pcie_lane(lane);

	kfree(pcie);
}

static void tegra210_pcie_lane_defaults(struct tegra_xusb_lane *lane)
{
	u32 reg;
	int i;

	if (lane->function == 1) {
		for (i = 0; i < ARRAY_SIZE(usb3_lane_data); i++) {
			reg = 0;
			reg |= CFG_ADDR(usb3_lane_data[i].cfg_addr);
			reg |= CFG_WDATA(usb3_lane_data[i].cfg_wdata);
			reg |= CFG_RESET;
			reg |= CFG_WS;
			padctl_writel(lane->pad->padctl, reg,
				XUSB_PADCTL_UPHY_MISC_PAD_PX_CTL8(lane->index));
		}
	} else if (lane->function == 0 || lane->function == 3) {
		for (i = 0; i < ARRAY_SIZE(pcie_lane_data); i++) {
			reg = 0;
			reg |= CFG_ADDR(pcie_lane_data[i].cfg_addr);
			reg |= CFG_WDATA(pcie_lane_data[i].cfg_wdata);
			reg |= CFG_RESET;
			reg |= CFG_WS;
			padctl_writel(lane->pad->padctl, reg,
				XUSB_PADCTL_UPHY_MISC_PAD_PX_CTL8(lane->index));
		}
	}
}

static const struct tegra_xusb_lane_ops tegra210_pcie_lane_ops = {
	.probe = tegra210_pcie_lane_probe,
	.remove = tegra210_pcie_lane_remove,
};

static int tegra210_pcie_phy_init(struct phy *phy)
{
	struct tegra_xusb_lane *lane = phy_get_drvdata(phy);

	dev_dbg(lane->pad->padctl->dev, "phy init lane = %s\n",
		lane->pad->soc->lanes[lane->index].name);

	return tegra210_xusb_padctl_enable(lane->pad->padctl);
}

static int tegra210_pcie_phy_exit(struct phy *phy)
{
	struct tegra_xusb_lane *lane = phy_get_drvdata(phy);

	return tegra210_xusb_padctl_disable(lane->pad->padctl);
}

static int tegra210_pcie_phy_power_on(struct phy *phy)
{
	struct tegra_xusb_lane *lane = phy_get_drvdata(phy);
	struct tegra_xusb_padctl *padctl = lane->pad->padctl;
	struct tegra210_xusb_padctl *priv = to_tegra210_xusb_padctl(padctl);
	u32 value;
	int err = 0;
	struct tegra_xusb_usb3_port *port;

	dev_dbg(padctl->dev, "phy power on lane = %s\n",
		lane->pad->soc->lanes[lane->index].name);

	mutex_lock(&padctl->lock);

	if (tegra_xusb_lane_check(lane, "xusb") && priv->prod_list) {
		char prod_name[] = "prod_c_ssX";

		port = tegra_xusb_find_usb3_port(padctl,
						tegra210_usb3_lane_map(lane));
		if (!port) {
			dev_err(&phy->dev, "no port found for USB3 lane %u\n",
						lane->index);
			mutex_unlock(&padctl->lock);
			return -ENODEV;
		}

		sprintf(prod_name, "prod_c_ss%d", port->base.index);
		err = tegra_prod_set_by_name(&padctl->regs, prod_name,
							priv->prod_list);
		if (err)
			dev_dbg(&phy->dev,
				"failed to apply prod for ss pad%d\n",
							port->base.index);
	}

	if (t210b01_compatible(padctl) == 1)
		tegra210_pcie_lane_defaults(lane);

	value = padctl_readl(padctl, XUSB_PADCTL_USB3_PAD_MUX);
	value |= XUSB_PADCTL_USB3_PAD_MUX_PCIE_IDDQ_DISABLE(lane->index);
	padctl_writel(padctl, value, XUSB_PADCTL_USB3_PAD_MUX);

	mutex_unlock(&padctl->lock);
	return err;
}

static int tegra210_pcie_phy_power_off(struct phy *phy)
{
	struct tegra_xusb_lane *lane = phy_get_drvdata(phy);
	struct tegra_xusb_padctl *padctl = lane->pad->padctl;
	u32 value;

	dev_dbg(padctl->dev, "phy power off lane = %s\n",
		lane->pad->soc->lanes[lane->index].name);

	mutex_lock(&padctl->lock);

	value = padctl_readl(padctl, XUSB_PADCTL_USB3_PAD_MUX);
	value &= ~XUSB_PADCTL_USB3_PAD_MUX_PCIE_IDDQ_DISABLE(lane->index);
	padctl_writel(padctl, value, XUSB_PADCTL_USB3_PAD_MUX);

	mutex_unlock(&padctl->lock);
	return 0;
}

static int tegra210_usb3_phy_enable_sleepwalk(struct phy *phy)
{
	struct tegra_xusb_lane *lane = phy_get_drvdata(phy);
	struct tegra_xusb_padctl *padctl = lane->pad->padctl;
	unsigned int index = tegra210_usb3_lane_map(lane);
	struct device *dev = padctl->dev;
	u32 reg;

	dev_dbg(dev, "phy enable sleepwalk on usb3-%d\n", index);

	mutex_lock(&padctl->lock);

	reg = padctl_readl(padctl, XUSB_PADCTL_ELPG_PROGRAM_1);
	reg |= SSPX_ELPG_CLAMP_EN_EARLY(index);
	padctl_writel(padctl, reg, XUSB_PADCTL_ELPG_PROGRAM_1);

	usleep_range(100, 200);

	reg = padctl_readl(padctl, XUSB_PADCTL_ELPG_PROGRAM_1);
	reg |= SSPX_ELPG_CLAMP_EN(index);
	padctl_writel(padctl, reg, XUSB_PADCTL_ELPG_PROGRAM_1);

	usleep_range(250, 350);

	mutex_unlock(&padctl->lock);

	return 0;
}

static int tegra210_usb3_phy_disable_sleepwalk(struct phy *phy)
{
	struct tegra_xusb_lane *lane = phy_get_drvdata(phy);
	struct tegra_xusb_padctl *padctl = lane->pad->padctl;
	unsigned int index = tegra210_usb3_lane_map(lane);
	struct device *dev = padctl->dev;
	u32 reg;

	dev_dbg(dev, "phy disable sleepwalk on usb3-%d\n", index);

	mutex_lock(&padctl->lock);

	reg = padctl_readl(padctl, XUSB_PADCTL_ELPG_PROGRAM_1);
	reg &= ~SSPX_ELPG_CLAMP_EN_EARLY(index);
	padctl_writel(padctl, reg, XUSB_PADCTL_ELPG_PROGRAM_1);

	usleep_range(100, 200);

	reg = padctl_readl(padctl, XUSB_PADCTL_ELPG_PROGRAM_1);
	reg &= ~SSPX_ELPG_CLAMP_EN(index);
	padctl_writel(padctl, reg, XUSB_PADCTL_ELPG_PROGRAM_1);

	mutex_unlock(&padctl->lock);

	return 0;
}

static int tegra210_usb3_phy_enable_wake(struct phy *phy)
{
	struct tegra_xusb_lane *lane = phy_get_drvdata(phy);
	struct tegra_xusb_padctl *padctl = lane->pad->padctl;
	unsigned int index = tegra210_usb3_lane_map(lane);
	struct device *dev = padctl->dev;
	u32 reg;

	dev_dbg(dev, "phy enable wake on usb3-%d\n", index);

	mutex_lock(&padctl->lock);

	reg = padctl_readl(padctl, XUSB_PADCTL_ELPG_PROGRAM_0);
	reg &= ~ALL_WAKE_EVENTS;
	reg |= SS_PORT_WAKEUP_EVENT(index);
	padctl_writel(padctl, reg, XUSB_PADCTL_ELPG_PROGRAM_0);

	usleep_range(10, 20);

	reg = padctl_readl(padctl, XUSB_PADCTL_ELPG_PROGRAM_0);
	reg &= ~ALL_WAKE_EVENTS;
	reg |= SS_PORT_WAKE_INTERRUPT_ENABLE(index);
	padctl_writel(padctl, reg, XUSB_PADCTL_ELPG_PROGRAM_0);

	mutex_unlock(&padctl->lock);

	return 0;
}

static int tegra210_usb3_phy_disable_wake(struct phy *phy)
{
	struct tegra_xusb_lane *lane = phy_get_drvdata(phy);
	struct tegra_xusb_padctl *padctl = lane->pad->padctl;
	unsigned int index = tegra210_usb3_lane_map(lane);
	struct device *dev = padctl->dev;
	u32 reg;

	dev_dbg(dev, "phy disable wake on usb3-%d\n", index);

	mutex_lock(&padctl->lock);

	reg = padctl_readl(padctl, XUSB_PADCTL_ELPG_PROGRAM_0);
	reg &= ~ALL_WAKE_EVENTS;
	reg &= ~SS_PORT_WAKE_INTERRUPT_ENABLE(index);
	padctl_writel(padctl, reg, XUSB_PADCTL_ELPG_PROGRAM_0);

	usleep_range(10, 20);

	reg = padctl_readl(padctl, XUSB_PADCTL_ELPG_PROGRAM_0);
	reg &= ~ALL_WAKE_EVENTS;
	reg |= SS_PORT_WAKEUP_EVENT(index);
	padctl_writel(padctl, reg, XUSB_PADCTL_ELPG_PROGRAM_0);

	mutex_unlock(&padctl->lock);

	return 0;
}

static const struct phy_ops tegra210_pcie_phy_ops = {
	.init = tegra210_pcie_phy_init,
	.exit = tegra210_pcie_phy_exit,
	.power_on = tegra210_pcie_phy_power_on,
	.power_off = tegra210_pcie_phy_power_off,
	.owner = THIS_MODULE,
};

static struct tegra_xusb_pad *
tegra210_pcie_pad_probe(struct tegra_xusb_padctl *padctl,
			const struct tegra_xusb_pad_soc *soc,
			struct device_node *np)
{
	struct tegra210_xusb_padctl *priv = to_tegra210_xusb_padctl(padctl);
	struct tegra_xusb_pcie_pad *pcie;
	struct tegra_xusb_pad *pad;
	int err;

	pcie = kzalloc(sizeof(*pcie), GFP_KERNEL);
	if (!pcie)
		return ERR_PTR(-ENOMEM);

	pad = &pcie->base;
	pad->ops = &tegra210_pcie_lane_ops;
	pad->soc = soc;

	err = tegra_xusb_pad_init(pad, padctl, np);
	if (err < 0) {
		kfree(pcie);
		goto out;
	}

	priv->plle = devm_clk_get(&pad->dev, "pll");
	if (IS_ERR(priv->plle)) {
		err = PTR_ERR(priv->plle);
		dev_err(&pad->dev, "failed to get PLLE: %d\n", err);
		goto unregister;
	}

	if (t210b01_compatible(padctl) == 1) {
		priv->uphy_mgmt_clk = devm_clk_get(&pad->dev, "uphy_mgmt");
		if (IS_ERR(priv->uphy_mgmt_clk)) {
			err = PTR_ERR(priv->uphy_mgmt_clk);
			dev_err(&pad->dev,
				"failed to get uphy_mgmt_clk clock: %d\n", err);
		}
	}

	pcie->rst = devm_reset_control_get(&pad->dev, "phy");
	if (IS_ERR(pcie->rst)) {
		err = PTR_ERR(pcie->rst);
		dev_err(&pad->dev, "failed to get PCIe pad reset: %d\n", err);
		goto unregister;
	}

	err = tegra_xusb_pad_register(pad, &tegra210_pcie_phy_ops);
	if (err < 0)
		goto unregister;

	dev_set_drvdata(&pad->dev, pad);

	return pad;

unregister:
	device_unregister(&pad->dev);
out:
	return ERR_PTR(err);
}

static void tegra210_pcie_pad_remove(struct tegra_xusb_pad *pad)
{
	struct tegra_xusb_pcie_pad *pcie = to_pcie_pad(pad);

	kfree(pcie);
}

static const struct tegra_xusb_pad_ops tegra210_pcie_ops = {
	.probe = tegra210_pcie_pad_probe,
	.remove = tegra210_pcie_pad_remove,
};

static const struct tegra_xusb_pad_soc tegra210_pcie_pad = {
	.name = "pcie",
	.num_lanes = ARRAY_SIZE(tegra210_pcie_lanes),
	.lanes = tegra210_pcie_lanes,
	.ops = &tegra210_pcie_ops,
};

static const struct tegra_xusb_pad_soc tegra210b01_pcie_pad = {
	.name = "pcie",
	.num_lanes = ARRAY_SIZE(tegra210b01_pcie_lanes),
	.lanes = tegra210b01_pcie_lanes,
	.ops = &tegra210_pcie_ops,
};

static const struct tegra_xusb_lane_soc tegra210_sata_lanes[] = {
	TEGRA210_LANE("sata-0", 0x028, 30, 0x3, pcie),
};

static struct tegra_xusb_lane *
tegra210_sata_lane_probe(struct tegra_xusb_pad *pad, struct device_node *np,
			 unsigned int index)
{
	struct tegra210_xusb_padctl *priv =
				to_tegra210_xusb_padctl(pad->padctl);
	struct tegra_xusb_sata_lane *sata;
	int err;

	sata = kzalloc(sizeof(*sata), GFP_KERNEL);
	if (!sata)
		return ERR_PTR(-ENOMEM);

	INIT_LIST_HEAD(&sata->base.list);
	sata->base.soc = &pad->soc->lanes[index];
	sata->base.index = index;
	sata->base.pad = pad;
	sata->base.np = np;

	err = tegra_xusb_lane_parse_dt(&sata->base, np);
	if (err < 0) {
		kfree(sata);
		return ERR_PTR(err);
	}

	if (tegra_xusb_lane_check(&sata->base, "xusb"))
		priv->sata_used_by_xusb = true;
	else
		priv->sata_used_by_xusb = false;

	dev_info(pad->padctl->dev, "dev = %s, lane = %s, function = %s\n",
		dev_name(&pad->lanes[index]->dev), pad->soc->lanes[index].name,
		sata->base.soc->funcs[sata->base.function]);

	return &sata->base;
}

static void tegra210_sata_lane_remove(struct tegra_xusb_lane *lane)
{
	struct tegra_xusb_sata_lane *sata = to_sata_lane(lane);

	kfree(sata);
}

static const struct tegra_xusb_lane_ops tegra210_sata_lane_ops = {
	.probe = tegra210_sata_lane_probe,
	.remove = tegra210_sata_lane_remove,
};

static int tegra210_sata_phy_init(struct phy *phy)
{
	struct tegra_xusb_lane *lane = phy_get_drvdata(phy);

	dev_dbg(lane->pad->padctl->dev, "phy init lane = %s\n",
		lane->pad->soc->lanes[lane->index].name);

	return tegra210_xusb_padctl_enable(lane->pad->padctl);
}

static int tegra210_sata_phy_exit(struct phy *phy)
{
	struct tegra_xusb_lane *lane = phy_get_drvdata(phy);

	return tegra210_xusb_padctl_disable(lane->pad->padctl);
}

static int tegra210_sata_phy_power_on(struct phy *phy)
{
	struct tegra_xusb_lane *lane = phy_get_drvdata(phy);
	struct tegra_xusb_padctl *padctl = lane->pad->padctl;
	u32 value;

	dev_dbg(padctl->dev, "phy power on lane = %s\n",
		lane->pad->soc->lanes[lane->index].name);

	mutex_lock(&padctl->lock);

	value = padctl_readl(padctl, XUSB_PADCTL_USB3_PAD_MUX);
	value |= XUSB_PADCTL_USB3_PAD_MUX_SATA_IDDQ_DISABLE(lane->index);
	padctl_writel(padctl, value, XUSB_PADCTL_USB3_PAD_MUX);

	mutex_unlock(&padctl->lock);
	return 0;
}

static int tegra210_sata_phy_power_off(struct phy *phy)
{
	struct tegra_xusb_lane *lane = phy_get_drvdata(phy);
	struct tegra_xusb_padctl *padctl = lane->pad->padctl;
	u32 value;

	dev_dbg(padctl->dev, "phy power off lane = %s\n",
		lane->pad->soc->lanes[lane->index].name);

	mutex_lock(&padctl->lock);

	value = padctl_readl(padctl, XUSB_PADCTL_USB3_PAD_MUX);
	value &= ~XUSB_PADCTL_USB3_PAD_MUX_SATA_IDDQ_DISABLE(lane->index);
	padctl_writel(padctl, value, XUSB_PADCTL_USB3_PAD_MUX);

	mutex_unlock(&padctl->lock);
	return 0;
}

static const struct phy_ops tegra210_sata_phy_ops = {
	.init = tegra210_sata_phy_init,
	.exit = tegra210_sata_phy_exit,
	.power_on = tegra210_sata_phy_power_on,
	.power_off = tegra210_sata_phy_power_off,
	.owner = THIS_MODULE,
};

static struct tegra_xusb_pad *
tegra210_sata_pad_probe(struct tegra_xusb_padctl *padctl,
			const struct tegra_xusb_pad_soc *soc,
			struct device_node *np)
{
	struct tegra_xusb_sata_pad *sata;
	struct tegra_xusb_pad *pad;
	int err;

	sata = kzalloc(sizeof(*sata), GFP_KERNEL);
	if (!sata)
		return ERR_PTR(-ENOMEM);

	pad = &sata->base;
	pad->ops = &tegra210_sata_lane_ops;
	pad->soc = soc;

	err = tegra_xusb_pad_init(pad, padctl, np);
	if (err < 0) {
		kfree(sata);
		goto out;
	}

	sata->rst = devm_reset_control_get(&pad->dev, "phy");
	if (IS_ERR(sata->rst)) {
		err = PTR_ERR(sata->rst);
		dev_err(&pad->dev, "failed to get SATA pad reset: %d\n", err);
		goto unregister;
	}

	err = tegra_xusb_pad_register(pad, &tegra210_sata_phy_ops);
	if (err < 0)
		goto unregister;

	dev_set_drvdata(&pad->dev, pad);

	return pad;

unregister:
	device_unregister(&pad->dev);
out:
	return ERR_PTR(err);
}

static void tegra210_sata_pad_remove(struct tegra_xusb_pad *pad)
{
	struct tegra_xusb_sata_pad *sata = to_sata_pad(pad);

	kfree(sata);
}

static const struct tegra_xusb_pad_ops tegra210_sata_ops = {
	.probe = tegra210_sata_pad_probe,
	.remove = tegra210_sata_pad_remove,
};

static const struct tegra_xusb_pad_soc tegra210_sata_pad = {
	.name = "sata",
	.num_lanes = ARRAY_SIZE(tegra210_sata_lanes),
	.lanes = tegra210_sata_lanes,
	.ops = &tegra210_sata_ops,
};

static const struct tegra_xusb_pad_soc * const tegra210_pads[] = {
	&tegra210_usb2_pad,
	&tegra210_hsic_pad,
	&tegra210_pcie_pad,
	&tegra210_sata_pad,
};

static const struct tegra_xusb_pad_soc * const tegra210b01_pads[] = {
	&tegra210_usb2_pad,
	&tegra210b01_pcie_pad,
};

static int tegra210_usb2_port_enable(struct tegra_xusb_port *port)
{
	return 0;
}

static void tegra210_usb2_port_disable(struct tegra_xusb_port *port)
{
}

static struct tegra_xusb_lane *
tegra210_usb2_port_map(struct tegra_xusb_port *port)
{
	struct tegra_xusb_lane *lane =
		tegra_xusb_find_lane(port->padctl, "usb2", port->index);

	dev_dbg(port->padctl->dev, "port = %s map to lane = %s\n",
		dev_name(&port->dev),
		lane->pad->soc->lanes[lane->index].name);

	return lane;
}

static const struct tegra_xusb_port_ops tegra210_usb2_port_ops = {
	.enable = tegra210_usb2_port_enable,
	.disable = tegra210_usb2_port_disable,
	.map = tegra210_usb2_port_map,
};

static int tegra210_hsic_port_enable(struct tegra_xusb_port *port)
{
	return 0;
}

static void tegra210_hsic_port_disable(struct tegra_xusb_port *port)
{
}

static struct tegra_xusb_lane *
tegra210_hsic_port_map(struct tegra_xusb_port *port)
{
	return tegra_xusb_find_lane(port->padctl, "hsic", port->index);
}

static const struct tegra_xusb_port_ops tegra210_hsic_port_ops = {
	.enable = tegra210_hsic_port_enable,
	.disable = tegra210_hsic_port_disable,
	.map = tegra210_hsic_port_map,
};

/* must be called under padctl->lock */
static int tegra210_usb3_port_enable(struct tegra_xusb_port *port)
{
	struct tegra_xusb_usb3_port *usb3 = to_usb3_port(port);
	struct tegra_xusb_padctl *padctl = port->padctl;
	struct tegra_xusb_lane *lane = usb3->base.lane;
	unsigned int index = port->index;
	u32 value;

	dev_dbg(padctl->dev, "enable usb3 port = %s\n",
		dev_name(&tegra_xusb_find_usb3_port(lane->pad->padctl,
			tegra210_usb3_lane_map(lane))->base.dev));

	value = padctl_readl(padctl, XUSB_PADCTL_SS_PORT_MAP);

	if (!usb3->internal)
		value &= ~XUSB_PADCTL_SS_PORT_MAP_PORTX_INTERNAL(index);
	else
		value |= XUSB_PADCTL_SS_PORT_MAP_PORTX_INTERNAL(index);

	value &= ~XUSB_PADCTL_SS_PORT_MAP_PORTX_MAP_MASK(index);
	value |= XUSB_PADCTL_SS_PORT_MAP_PORTX_MAP(index, usb3->port);
	padctl_writel(padctl, value, XUSB_PADCTL_SS_PORT_MAP);

	value = padctl_readl(padctl, XUSB_PADCTL_UPHY_USB3_PADX_ECTL1(index));
	value &= ~(XUSB_PADCTL_UPHY_USB3_PAD_ECTL1_TX_TERM_CTRL_MASK <<
		   XUSB_PADCTL_UPHY_USB3_PAD_ECTL1_TX_TERM_CTRL_SHIFT);
	value |= XUSB_PADCTL_UPHY_USB3_PAD_ECTL1_TX_TERM_CTRL_VAL <<
		 XUSB_PADCTL_UPHY_USB3_PAD_ECTL1_TX_TERM_CTRL_SHIFT;
	padctl_writel(padctl, value, XUSB_PADCTL_UPHY_USB3_PADX_ECTL1(index));

	value = padctl_readl(padctl, XUSB_PADCTL_UPHY_USB3_PADX_ECTL2(index));
	value &= ~(XUSB_PADCTL_UPHY_USB3_PAD_ECTL2_RX_CTLE_MASK <<
		   XUSB_PADCTL_UPHY_USB3_PAD_ECTL2_RX_CTLE_SHIFT);
	value |= XUSB_PADCTL_UPHY_USB3_PAD_ECTL2_RX_CTLE_VAL <<
		 XUSB_PADCTL_UPHY_USB3_PAD_ECTL2_RX_CTLE_SHIFT;
	padctl_writel(padctl, value, XUSB_PADCTL_UPHY_USB3_PADX_ECTL2(index));

	padctl_writel(padctl, XUSB_PADCTL_UPHY_USB3_PAD_ECTL3_RX_DFE_VAL,
		      XUSB_PADCTL_UPHY_USB3_PADX_ECTL3(index));

	value = padctl_readl(padctl, XUSB_PADCTL_UPHY_USB3_PADX_ECTL4(index));
	value &= ~(XUSB_PADCTL_UPHY_USB3_PAD_ECTL4_RX_CDR_CTRL_MASK <<
		   XUSB_PADCTL_UPHY_USB3_PAD_ECTL4_RX_CDR_CTRL_SHIFT);
	value |= XUSB_PADCTL_UPHY_USB3_PAD_ECTL4_RX_CDR_CTRL_VAL <<
		 XUSB_PADCTL_UPHY_USB3_PAD_ECTL4_RX_CDR_CTRL_SHIFT;
	padctl_writel(padctl, value, XUSB_PADCTL_UPHY_USB3_PADX_ECTL4(index));

	padctl_writel(padctl, XUSB_PADCTL_UPHY_USB3_PAD_ECTL6_RX_EQ_CTRL_H_VAL,
		      XUSB_PADCTL_UPHY_USB3_PADX_ECTL6(index));

	value = padctl_readl(padctl, XUSB_PADCTL_ELPG_PROGRAM_1);
	value &= ~SSPX_ELPG_VCORE_DOWN(index);
	padctl_writel(padctl, value, XUSB_PADCTL_ELPG_PROGRAM_1);

	usleep_range(100, 200);

	value = padctl_readl(padctl, XUSB_PADCTL_ELPG_PROGRAM_1);
	value &= ~SSPX_ELPG_CLAMP_EN_EARLY(index);
	padctl_writel(padctl, value, XUSB_PADCTL_ELPG_PROGRAM_1);

	usleep_range(100, 200);

	value = padctl_readl(padctl, XUSB_PADCTL_ELPG_PROGRAM_1);
	value &= ~SSPX_ELPG_CLAMP_EN(index);
	padctl_writel(padctl, value, XUSB_PADCTL_ELPG_PROGRAM_1);

	return 0;
}

/* must be called under padctl->lock */
static void tegra210_usb3_port_disable(struct tegra_xusb_port *port)
{
	struct tegra_xusb_padctl *padctl = port->padctl;
	struct tegra_xusb_lane *lane = port->lane;
	unsigned int index = port->index;
	u32 value;

	dev_dbg(padctl->dev, "disable usb3 port = %s\n",
		dev_name(&tegra_xusb_find_usb3_port(lane->pad->padctl,
			tegra210_usb3_lane_map(lane))->base.dev));

	value = padctl_readl(padctl, XUSB_PADCTL_ELPG_PROGRAM_1);
	value |= SSPX_ELPG_CLAMP_EN_EARLY(index);
	padctl_writel(padctl, value, XUSB_PADCTL_ELPG_PROGRAM_1);

	usleep_range(100, 200);

	value = padctl_readl(padctl, XUSB_PADCTL_ELPG_PROGRAM_1);
	value |= SSPX_ELPG_CLAMP_EN(index);
	padctl_writel(padctl, value, XUSB_PADCTL_ELPG_PROGRAM_1);

	usleep_range(250, 350);

	value = padctl_readl(padctl, XUSB_PADCTL_ELPG_PROGRAM_1);
	value |= SSPX_ELPG_VCORE_DOWN(index);
	padctl_writel(padctl, value, XUSB_PADCTL_ELPG_PROGRAM_1);

	value = padctl_readl(padctl, XUSB_PADCTL_SS_PORT_MAP);
	value &= ~XUSB_PADCTL_SS_PORT_MAP_PORTX_MAP_MASK(index);
	value |= XUSB_PADCTL_SS_PORT_MAP_PORTX_MAP(index, 0x7);
	padctl_writel(padctl, value, XUSB_PADCTL_SS_PORT_MAP);
}

static const struct tegra_xusb_lane_map tegra210_usb3_map[] = {
	{ 0, "pcie", 6 },
	{ 1, "pcie", 5 },
	{ 2, "pcie", 0 },
	{ 2, "pcie", 3 },
	{ 3, "pcie", 4 },
	{ 3, "sata", 0 },
	{ 0, NULL,   0 }
};

static const struct tegra_xusb_lane_map tegra210b01_usb3_map[] = {
	{ 0, "pcie", 5 },
	{ 1, "pcie", 4 },
	{ 2, "pcie", 1 },
	{ 0, NULL,   0 }
};

static struct tegra_xusb_lane *
tegra210_usb3_port_map(struct tegra_xusb_port *port)
{
	struct tegra_xusb_lane *lane;
	int err = t210b01_compatible(port->padctl);

	if (err == 1)
		lane = tegra_xusb_port_find_lane(port,
						tegra210b01_usb3_map, "xusb");
	else
		lane = tegra_xusb_port_find_lane(port,
						tegra210_usb3_map, "xusb");

	dev_dbg(port->padctl->dev, "port = %s map to lane = %s\n",
			dev_name(&port->dev),
			lane->pad->soc->lanes[lane->index].name);

	return lane;
}

static const struct tegra_xusb_port_ops tegra210_usb3_port_ops = {
	.enable = tegra210_usb3_port_enable,
	.disable = tegra210_usb3_port_disable,
	.map = tegra210_usb3_port_map,
};

unsigned int
tegra210_usb3_lane_find_port_index(struct tegra_xusb_lane *lane,
				const struct tegra_xusb_lane_map *map,
				const char *function)
{
	for (map = map; map->type; map++) {
		if (map->index == lane->index &&
			strcmp(map->type, lane->pad->soc->name) == 0) {
			dev_dbg(lane->pad->padctl->dev,
				"lane = %s map to port = usb3-%d\n",
				lane->pad->soc->lanes[lane->index].name,
				map->port);
			return map->port;
		}
	}

	return -1;
}

static unsigned int
tegra210_usb3_lane_map(struct tegra_xusb_lane *lane)
{
	int err = t210b01_compatible(lane->pad->padctl);

	if (err == 1)
		return tegra210_usb3_lane_find_port_index(lane,
					tegra210b01_usb3_map, "xusb");
	else if (err == 0)
		return tegra210_usb3_lane_find_port_index(lane,
					tegra210_usb3_map, "xusb");
	else
		return err;
}

static inline bool is_usb3_phy(struct phy *phy)
{
	return phy->ops == &tegra210_pcie_phy_ops;
}

static bool is_usb3_phy_has_otg_cap(struct tegra_xusb_padctl *padctl,
				struct phy *phy)
{
		struct tegra_xusb_lane *lane;
		unsigned int index;
		struct tegra_xusb_usb3_port *port;

		if (!phy)
			return false;

		lane = phy_get_drvdata(phy);
		index = tegra210_usb3_lane_map(lane);

		port = tegra_xusb_find_usb3_port(padctl, index);
		if (!port) {
			dev_err(padctl->dev, "no port found for USB3 lane %u\n",
					index);
			return false;
		}

	return port->port_cap == USB_OTG_CAP;
}

static bool tegra210_xusb_padctl_has_otg_cap(struct tegra_xusb_padctl *padctl,
				struct phy *phy)
{
	if (is_utmi_phy(phy))
		return is_utmi_phy_has_otg_cap(padctl, phy);
	else if (is_usb3_phy(phy))
		return is_usb3_phy_has_otg_cap(padctl, phy);

	return false;
}

static int tegra210_xusb_padctl_vbus_override(struct tegra_xusb_padctl *padctl,
					      bool set)
{
	u32 reg;

	dev_dbg(padctl->dev, "%s vbus override\n", set ? "set" : "clear");

	reg = padctl_readl(padctl, XUSB_PADCTL_USB2_VBUS_ID);
	if (set) {
		reg |= VBUS_OVERRIDE_VBUS_ON;
		reg &= ~ID_OVERRIDE(~0);
		reg |= ID_OVERRIDE_FLOATING;
	} else
		reg &= ~VBUS_OVERRIDE_VBUS_ON;
	padctl_writel(padctl, reg, XUSB_PADCTL_USB2_VBUS_ID);

	schedule_work(&padctl->otg_vbus_work);
	return 0;
}

static int tegra210_xusb_padctl_id_override(struct tegra_xusb_padctl *padctl,
					 bool set)
{
	u32 reg;

	dev_dbg(padctl->dev, "%s id override\n", set ? "set" : "clear");

	reg = padctl_readl(padctl, XUSB_PADCTL_USB2_VBUS_ID);
	if (set) {
		if (reg & VBUS_OVERRIDE_VBUS_ON) {
			reg &= ~VBUS_OVERRIDE_VBUS_ON;
			padctl_writel(padctl, reg, XUSB_PADCTL_USB2_VBUS_ID);
			usleep_range(1000, 2000);

			reg = padctl_readl(padctl, XUSB_PADCTL_USB2_VBUS_ID);
		}

		reg &= ~ID_OVERRIDE(~0);
		reg |= ID_OVERRIDE_GROUNDED;
	} else {
		reg &= ~ID_OVERRIDE(~0);
		reg |= ID_OVERRIDE_FLOATING;
	}
	padctl_writel(padctl, reg, XUSB_PADCTL_USB2_VBUS_ID);

	schedule_work(&padctl->otg_vbus_work);

	return 0;
}

void tegra210_utmi_pad_power_on(struct phy *phy)
{
	struct tegra_xusb_lane *lane;
	struct tegra_xusb_usb2_lane *usb2;
	struct tegra_xusb_padctl *padctl;
	unsigned int index;
	struct device *dev;
	u32 reg;

	if (!phy)
		return;

	lane = phy_get_drvdata(phy);
	usb2 = to_usb2_lane(lane);
	padctl = lane->pad->padctl;
	index = lane->index;
	dev = padctl->dev;

	dev_info(dev, "power on UTMI pads %d\n", index);

	if (usb2->powered_on)
		return;

	//tegra210_utmi_bias_pad_power_on(padctl);

	udelay(2);

	reg = padctl_readl(padctl, XUSB_PADCTL_USB2_OTG_PADX_CTL0(index));
	reg &= ~XUSB_PADCTL_USB2_OTG_PAD_CTL0_PD;
	padctl_writel(padctl, reg, XUSB_PADCTL_USB2_OTG_PADX_CTL0(index));

	reg = padctl_readl(padctl, XUSB_PADCTL_USB2_OTG_PADX_CTL_1(index));
	reg &= ~USB2_OTG_PD_DR;
	padctl_writel(padctl, reg, XUSB_PADCTL_USB2_OTG_PADX_CTL_1(index));

	usb2->powered_on = true;
}

void tegra210_utmi_pad_power_down(struct phy *phy)
{
	struct tegra_xusb_lane *lane;
	struct tegra_xusb_usb2_lane *usb2;
	struct tegra_xusb_padctl *padctl;
	unsigned int index;
	struct device *dev;
	u32 reg;

	if (!phy)
		return;

	lane = phy_get_drvdata(phy);
	usb2 = to_usb2_lane(lane);
	padctl = lane->pad->padctl;
	index = lane->index;
	dev = padctl->dev;

	dev_info(dev, "power down UTMI pad %d\n", index);

	if (!usb2->powered_on)
		return;

	reg = padctl_readl(padctl, XUSB_PADCTL_USB2_OTG_PADX_CTL0(index));
	reg |= XUSB_PADCTL_USB2_OTG_PAD_CTL0_PD;
	padctl_writel(padctl, reg, XUSB_PADCTL_USB2_OTG_PADX_CTL0(index));

	reg = padctl_readl(padctl, XUSB_PADCTL_USB2_OTG_PADX_CTL_1(index));
	reg |= USB2_OTG_PD_DR;
	padctl_writel(padctl, reg, XUSB_PADCTL_USB2_OTG_PADX_CTL_1(index));

	udelay(2);

	//tegra210_utmi_bias_pad_power_off(padctl);
	usb2->powered_on = false;
}

static int tegra210_utmi_port_reset_quirk(struct phy *phy)
{
	struct tegra_xusb_padctl *padctl;
	struct tegra_xusb_lane *lane;
	struct device *dev;
	u32 reg;

	if (!phy)
		return -ENODEV;

	lane = phy_get_drvdata(phy);
	padctl = lane->pad->padctl;
	dev = padctl->dev;

	reg = padctl_readl(padctl,
				XUSB_PADCTL_USB2_BATTERY_CHRG_OTGPADX_CTL0(0));
	dev_dbg(dev, "BATTERY_CHRG_OTGPADX_CTL0(0): 0x%x\n", reg);

	if ((reg & ZIP) || (reg & ZIN)) {
		dev_dbg(dev, "Toggle vbus\n");
		tegra210_xusb_padctl_vbus_override(padctl, false);
		tegra210_xusb_padctl_vbus_override(padctl, true);
		return 1;
	}
	return 0;
}

static int
tegra210_xusb_read_fuse_calibration(struct tegra210_xusb_fuse_calibration *fuse)
{
	unsigned int i;
	u32 value;
	int err;

	err = tegra_fuse_readl(TEGRA_FUSE_SKU_CALIB_0, &value);
	if (err < 0)
		return err;

	for (i = 0; i < ARRAY_SIZE(fuse->hs_curr_level); i++) {
		fuse->hs_curr_level[i] =
			(value >> FUSE_SKU_CALIB_HS_CURR_LEVEL_PADX_SHIFT(i)) &
			FUSE_SKU_CALIB_HS_CURR_LEVEL_PAD_MASK;
	}

	fuse->hs_term_range_adj =
		(value >> FUSE_SKU_CALIB_HS_TERM_RANGE_ADJ_SHIFT) &
		FUSE_SKU_CALIB_HS_TERM_RANGE_ADJ_MASK;

	err = tegra_fuse_readl(TEGRA_FUSE_USB_CALIB_EXT_0, &value);
	if (err < 0)
		return err;

	fuse->rpd_ctrl =
		(value >> FUSE_USB_CALIB_EXT_RPD_CTRL_SHIFT) &
		FUSE_USB_CALIB_EXT_RPD_CTRL_MASK;

	return 0;
}

static struct tegra_xusb_padctl *
tegra210_xusb_padctl_probe(struct device *dev,
			   const struct tegra_xusb_padctl_soc *soc)
{
	struct tegra210_xusb_padctl *padctl;
	int err;

	padctl = devm_kzalloc(dev, sizeof(*padctl), GFP_KERNEL);
	if (!padctl)
		return ERR_PTR(-ENOMEM);

	padctl->base.dev = dev;
	padctl->base.soc = soc;

	err = tegra210_xusb_read_fuse_calibration(&padctl->fuse);
	if (err < 0)
		return ERR_PTR(err);

	padctl->prod_list = devm_tegra_prod_get(dev);
	if (IS_ERR(padctl->prod_list)) {
		dev_warn(dev, "Prod-settings is not available\n");
		padctl->prod_list = NULL;
	}

	return &padctl->base;
}

static void tegra210_xusb_padctl_remove(struct tegra_xusb_padctl *padctl)
{
}

static int tegra210_xusb_padctl_phy_sleepwalk(struct tegra_xusb_padctl *padctl,
					      struct phy *phy, bool enable,
					      enum usb_device_speed speed)
{
	struct tegra210_xusb_padctl *priv;
	struct tegra_xusb_lane *lane;

	if (!phy)
		return 0;

	priv = to_tegra210_xusb_padctl(padctl);
	lane = phy_get_drvdata(phy);

	if (is_usb3_phy(phy)) {
		if (enable)
			return tegra210_usb3_phy_enable_sleepwalk(phy);
		else
			return tegra210_usb3_phy_disable_sleepwalk(phy);
	} else if (is_utmi_phy(phy)) {
		tegra210_utmi_phy_get_pad_config(padctl, lane->index,
				&priv->utmi_pad_cfg);
		if (enable)
			return tegra_pmc_utmi_phy_enable_sleepwalk(
					lane->index, speed,
					&priv->utmi_pad_cfg);
		else
			return tegra_pmc_utmi_phy_disable_sleepwalk(
					lane->index);
	} else if (is_hsic_phy(phy)) {
		if (enable)
			return tegra_pmc_hsic_phy_enable_sleepwalk(
					lane->index);
		else
			return tegra_pmc_hsic_phy_disable_sleepwalk(
					lane->index);
	} else
		return -EINVAL;

	return 0;
}

static int tegra210_xusb_padctl_phy_wake(struct tegra_xusb_padctl *padctl,
					 struct phy *phy, bool enable)
{
	if (!phy)
		return 0;

	if (is_usb3_phy(phy)) {
		if (enable)
			return tegra210_usb3_phy_enable_wake(phy);
		else
			return tegra210_usb3_phy_disable_wake(phy);
	} else if (is_utmi_phy(phy)) {
		if (enable)
			return tegra210_utmi_phy_enable_wake(phy);
		else
			return tegra210_utmi_phy_disable_wake(phy);
	} else if (is_hsic_phy(phy)) {
		if (enable)
			return tegra210_hsic_phy_enable_wake(phy);
		else
			return tegra210_hsic_phy_disable_wake(phy);
	} else
		return -EINVAL;

	return 0;
}

static int tegra210_usb3_phy_remote_wake_detected(
			struct tegra_xusb_padctl *padctl, int port)
{
	u32 reg;

	reg = padctl_readl(padctl, XUSB_PADCTL_ELPG_PROGRAM_0);
	if ((reg & SS_PORT_WAKE_INTERRUPT_ENABLE(port)) &&
			(reg & SS_PORT_WAKEUP_EVENT(port)))
		return true;
	else
		return false;
}

static int tegra210_utmi_phy_remote_wake_detected(
			struct tegra_xusb_padctl *padctl, int port)
{
	u32 reg;

	reg = padctl_readl(padctl, XUSB_PADCTL_ELPG_PROGRAM_0);
	if ((reg & USB2_PORT_WAKE_INTERRUPT_ENABLE(port)) &&
			(reg & USB2_PORT_WAKEUP_EVENT(port)))
		return true;
	else
		return false;
}

static int tegra210_hsic_phy_remote_wake_detected(
			struct tegra_xusb_padctl *padctl, int port)
{
	u32 reg;

	dev_dbg(padctl->dev, "hsic-%d remote wake detected\n", port);

	reg = padctl_readl(padctl, XUSB_PADCTL_ELPG_PROGRAM_0);
	if ((reg & USB2_HSIC_PORT_WAKE_INTERRUPT_ENABLE(port)) &&
			(reg & USB2_HSIC_PORT_WAKEUP_EVENT(port)))
		return true;
	else
		return false;
}

int tegra210_xusb_padctl_remote_wake_detected(struct phy *phy)
{
	struct tegra_xusb_lane *lane;
	struct tegra_xusb_padctl *padctl;

	if (!phy)
		return 0;

	lane = phy_get_drvdata(phy);
	padctl = lane->pad->padctl;

	if (is_utmi_phy(phy))
		return tegra210_utmi_phy_remote_wake_detected(padctl,
					lane->index);
	else if (is_hsic_phy(phy))
		return tegra210_hsic_phy_remote_wake_detected(padctl,
					lane->index);
	else if (is_usb3_phy(phy))
		return tegra210_usb3_phy_remote_wake_detected(padctl,
					tegra210_usb3_lane_map(lane));

	return -EINVAL;
}

/* should only be called with a UTMI phy and with padctl->lock held */
static void tegra210_enable_vbus_oc(struct phy *phy)
{
	struct tegra_xusb_lane *lane = phy_get_drvdata(phy);
	struct tegra_xusb_padctl *padctl = lane->pad->padctl;

	dev_dbg(padctl->dev, "enable VBUS OC on %s\n",
			dev_name(&tegra_xusb_find_usb2_port(
					padctl, lane->index)->base.dev));

	/* TODO implement */
}

/* should only be called with a UTMI phy and with padctl->lock held */
static void tegra210_disable_vbus_oc(struct phy *phy)
{
	struct tegra_xusb_lane *lane = phy_get_drvdata(phy);
	struct tegra_xusb_padctl *padctl = lane->pad->padctl;

	dev_dbg(padctl->dev, "disable VBUS OC on %s\n",
			dev_name(&tegra_xusb_find_usb2_port(
					padctl, lane->index)->base.dev));

	/* TODO implement */
}

static int tegra210_xusb_padctl_vbus_power_on(struct tegra_xusb_padctl *padctl,
					unsigned int index)
{
	int rc = 0;
	int status;
	struct tegra_xusb_usb2_port *port;

	port = tegra_xusb_find_usb2_port(padctl, index);
	if (!port) {
		dev_err(padctl->dev, "no port found for USB2 lane %u\n", index);
		return -ENODEV;
	}

	if (!port->supply) {
		dev_err(padctl->dev, "no vbus-supply found for USB2-%u\n",
			index);
		return -ENODEV;
	}

	dev_dbg(padctl->dev, "power on VBUS on %s\n",
			dev_name(&port->base.dev));

	mutex_lock(&padctl->lock);

	if (padctl->oc_pinctrl && port->oc_pin >= 0) {
		rc = tegra_xusb_select_vbus_en_state(padctl,
						port->oc_pin, true);
		tegra210_enable_vbus_oc(padctl->usb2->lanes[index]);
	} else {
		status = regulator_is_enabled(port->supply);
		if (!status) {
			rc = regulator_enable(port->supply);
			if (rc)
				dev_err(padctl->dev,
				"enable usb2-%d vbus failed %d\n", index, rc);
		}

		dev_dbg(padctl->dev, "%s: usb2-%d vbus status: %d->%d\n",
			__func__, index, status,
			regulator_is_enabled(port->supply));
	}
	mutex_unlock(&padctl->lock);
	return rc;
}

static int tegra210_xusb_padctl_vbus_power_off(struct tegra_xusb_padctl *padctl,
					unsigned int index)
{
	int rc = 0;
	int status;
	struct tegra_xusb_usb2_port *port;

	port = tegra_xusb_find_usb2_port(padctl, index);
	if (!port) {
		dev_err(padctl->dev, "no port found for USB2 lane %u\n", index);
		return -ENODEV;
	}

	if (padctl->otg_vbus_alwayson) {
		dev_info(padctl->dev, "%s: usb2-%d vbus cannot off due to alwayson\n",
			__func__, index);
		return -EINVAL;
	}

	if (!port->supply) {
		dev_err(padctl->dev, "no vbus-supply found for USB2-%u\n",
			index);
		return -ENODEV;
	}

	dev_dbg(padctl->dev, "power off VBUS on %s\n",
			dev_name(&port->base.dev));

	mutex_lock(&padctl->lock);

	if (padctl->oc_pinctrl && port->oc_pin >= 0) {
		rc = tegra_xusb_select_vbus_en_state(padctl,
						port->oc_pin, false);
		tegra210_disable_vbus_oc(padctl->usb2->lanes[index]);
	} else {
		status = regulator_is_enabled(port->supply);
		if (status) {
			rc = regulator_disable(port->supply);
			if (rc)
				dev_err(padctl->dev,
					"disable usb2-%d vbus failed %d\n",
					index, rc);
		}

		dev_dbg(padctl->dev, "%s: usb2-%d vbus status: %d->%d\n",
			__func__, index, status,
			regulator_is_enabled(port->supply));
	}
	mutex_unlock(&padctl->lock);
	return rc;
}

static void tegra210_xusb_padctl_otg_vbus_handle
			(struct tegra_xusb_padctl *padctl, unsigned int index)
{
	u32 reg;
	int err;

	reg = padctl_readl(padctl, XUSB_PADCTL_USB2_VBUS_ID);
	dev_dbg(padctl->dev, "USB2_VBUS_ID 0x%x otg_vbus_on was %d\n", reg,
		padctl->otg_vbus_on);

	if ((reg & ID_OVERRIDE(~0)) == ID_OVERRIDE_GROUNDED) {
		/* entering host mode role */
		if (!padctl->otg_vbus_on) {
			err = tegra210_xusb_padctl_vbus_power_on(padctl, index);
			if (!err)
				padctl->otg_vbus_on = true;
		}
	} else if ((reg & ID_OVERRIDE(~0)) == ID_OVERRIDE_FLOATING) {
		/* leaving host mode role */
		if (padctl->otg_vbus_on) {
			err = tegra210_xusb_padctl_vbus_power_off(padctl,
								  index);
			if (!err)
				padctl->otg_vbus_on = false;
		}
	}
}

static const struct tegra_xusb_padctl_ops tegra210_xusb_padctl_ops = {
	.probe = tegra210_xusb_padctl_probe,
	.remove = tegra210_xusb_padctl_remove,
	.phy_sleepwalk = tegra210_xusb_padctl_phy_sleepwalk,
	.phy_wake = tegra210_xusb_padctl_phy_wake,
	.remote_wake_detected = tegra210_xusb_padctl_remote_wake_detected,
	.vbus_power_on = tegra210_xusb_padctl_vbus_power_on,
	.vbus_power_off = tegra210_xusb_padctl_vbus_power_off,
	.otg_vbus_handle = tegra210_xusb_padctl_otg_vbus_handle,
	.usb3_set_lfps_detect = tegra210_usb3_set_lfps_detect,
	.hsic_set_idle = tegra210_hsic_set_idle,
	.has_otg_cap = tegra210_xusb_padctl_has_otg_cap,
	.vbus_override = tegra210_xusb_padctl_vbus_override,
	.id_override = tegra210_xusb_padctl_id_override,
	.utmi_pad_power_on = tegra210_utmi_pad_power_on,
	.utmi_pad_power_down = tegra210_utmi_pad_power_down,
	.utmi_port_reset_quirk = tegra210_utmi_port_reset_quirk,
};

static const char * const tegra210_supply_names[] = {
	"avdd_pll_uerefe",
	"hvdd_pex_pll_e",
	"dvdd_pex_pll",
	"hvddio_pex",
	"dvddio_pex",
	"hvdd_sata",
	"dvdd_sata_pll",
	"hvddio_sata",
	"dvddio_sata",
};

static const char * const tegra210b01_supply_names[] = {
	"avdd_pll_uerefe",
	"hvdd_pex_pll_e",
	"dvdd_pex_pll",
	"hvddio_pex",
	"dvddio_pex",
};

const struct tegra_xusb_padctl_soc tegra210_xusb_padctl_soc = {
	.num_pads = ARRAY_SIZE(tegra210_pads),
	.pads = tegra210_pads,
	.ports = {
		.usb2 = {
			.ops = &tegra210_usb2_port_ops,
			.count = 4,
		},
		.hsic = {
			.ops = &tegra210_hsic_port_ops,
			.count = 1,
		},
		.usb3 = {
			.ops = &tegra210_usb3_port_ops,
			.count = 4,
		},
	},
	.ops = &tegra210_xusb_padctl_ops,
	.supply_names = tegra210_supply_names,
	.num_supplies = ARRAY_SIZE(tegra210_supply_names),
};
EXPORT_SYMBOL_GPL(tegra210_xusb_padctl_soc);

const struct tegra_xusb_padctl_soc tegra210b01_xusb_padctl_soc = {
	.num_pads = ARRAY_SIZE(tegra210b01_pads),
	.pads = tegra210b01_pads,
	.ports = {
		.usb2 = {
			.ops = &tegra210_usb2_port_ops,
			.count = 4,
		},
		.usb3 = {
			.ops = &tegra210_usb3_port_ops,
			.count = 4,
		},
	},
	.ops = &tegra210_xusb_padctl_ops,
	.supply_names = tegra210b01_supply_names,
	.num_supplies = ARRAY_SIZE(tegra210b01_supply_names),
};
EXPORT_SYMBOL_GPL(tegra210b01_xusb_padctl_soc);

MODULE_AUTHOR("Andrew Bresticker <abrestic@chromium.org>");
MODULE_DESCRIPTION("NVIDIA Tegra 210 XUSB Pad Controller driver");
MODULE_LICENSE("GPL v2");
