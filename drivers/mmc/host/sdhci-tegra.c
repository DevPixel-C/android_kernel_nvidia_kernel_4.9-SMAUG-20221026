/*
 * Copyright (C) 2010 Google, Inc.
 * Copyright (c) 2012-2017, NVIDIA CORPORATION.  All rights reserved.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/delay.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/clk.h>
#include <linux/reset.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/mmc/card.h>
#include <linux/mmc/host.h>
#include <linux/mmc/mmc.h>
#include <linux/mmc/slot-gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/debugfs.h>
#include <linux/stat.h>
#include <linux/padctrl/padctrl.h>
#include <linux/regulator/consumer.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/dma-mapping.h>
#include <linux/mmc/cmdq_hci.h>

#include <linux/tegra_prod.h>
#include <linux/tegra-soc.h>
#include "sdhci-pltfm.h"

/* Tegra SDHOST controller vendor register definitions */
#define SDHCI_TEGRA_VENDOR_CLOCK_CTRL			0x100
#define SDHCI_CLOCK_CTRL_TAP_MASK			0x00ff0000
#define SDHCI_CLOCK_CTRL_TAP_SHIFT			16
#define SDHCI_CLOCK_CTRL_TRIM_SHIFT			24
#define SDHCI_CLOCK_CTRL_TRIM_MASK			0x1F
#define SDHCI_CLOCK_CTRL_SDR50_TUNING_OVERRIDE		BIT(5)
#define SDHCI_CLOCK_CTRL_PADPIPE_CLKEN_OVERRIDE		BIT(3)
#define SDHCI_CLOCK_CTRL_SPI_MODE_CLKEN_OVERRIDE	BIT(2)
#define SDHCI_CLOCK_CTRL_SDMMC_CLK			BIT(0)

#define SDHCI_TEGRA_VENDOR_SYS_SW_CTRL		0x104
#define SDHCI_SYS_SW_CTRL_STROBE_EN		0x80000000

#define SDHCI_TEGRA_VENDOR_ERR_INTR_STATUS	0x108

#define SDHCI_TEGRA_VENDOR_CAP_OVERRIDES	0x10C
#define SDHCI_VENDOR_CAP_DQS_TRIM_SHIFT		0x8
#define SDHCI_VENDOR_CAP_DQS_TRIM_MASK		0x3F

#define SDHCI_TEGRA_VENDOR_MISC_CTRL		0x120
#define SDHCI_MISC_CTRL_ENABLE_SDR104		0x8
#define SDHCI_MISC_CTRL_ENABLE_SDR50		0x10
#define SDHCI_MISC_CTRL_ENABLE_SDHCI_SPEC_300	0x20
#define SDHCI_MISC_CTRL_ENABLE_DDR50		0x200

#define SDHCI_TEGRA_VENDOR_MISC_CTRL_1		0x124

#define SDHCI_TEGRA_VENDOR_MISC_CTRL_2		0x128

#define SDMMC_VNDR_IO_TRIM_CTRL_0		0x1AC
#define SDMMC_VNDR_IO_TRIM_CTRL_0_SEL_VREG_MASK	0x4

#define SDHCI_TEGRA_VENDOR_DLLCAL_CFG		0x1B0
#define SDHCI_DLLCAL_CFG_EN_CALIBRATE		0x80000000

#define SDHCI_DLLCAL_CFG_STATUS			0x1BC
#define SDHCI_DLLCAL_CFG_STATUS_DLL_ACTIVE	0x80000000

#define SDHCI_VNDR_TUN_CTRL0_0			0x1c0
#define SDHCI_VNDR_TUN_CTRL0_TUN_HW_TAP		0x20000
#define SDHCI_TUN_CTRL0_TUNING_ITER_MASK	0x7
#define SDHCI_TUN_CTRL0_TUNING_ITER_SHIFT	13
#define SDHCI_TUN_CTRL0_TUNING_WORD_SEL_MASK	0x7
#define SDHCI_VNDR_TUN_CTRL0_0_TUN_ITER_MASK	0x000E000
#define TUNING_WORD_SEL_MASK	0x7

#define SDHCI_TEGRA_VNDR_TUNING_STATUS0		0x1C8

#define SDHCI_TEGRA_SDMEM_COMP_PADCTRL		0x1E0
#define SDHCI_TEGRA_PAD_E_INPUT_OR_E_PWRD_MASK	0x80000000
#define SDHCI_TEGRA_SDMEMCOMP_PADCTRL_VREF_SEL	0x0000000F

#define SDHCI_TEGRA_AUTO_CAL_CONFIG		0x1e4
#define SDHCI_AUTO_CAL_START			BIT(31)
#define SDHCI_AUTO_CAL_ENABLE			BIT(29)
#define SDHCI_AUTO_CAL_PUPD_OFFSETS		0x00007F7F

#define SDHCI_TEGRA_AUTO_CAL_STATUS	0x1EC
#define SDHCI_TEGRA_AUTO_CAL_ACTIVE	0x80000000

#define NVQUIRK_FORCE_SDHCI_SPEC_200	BIT(0)
#define NVQUIRK_ENABLE_BLOCK_GAP_DET	BIT(1)
#define NVQUIRK_ENABLE_SDHCI_SPEC_300	BIT(2)
#define NVQUIRK_ENABLE_SDR50		BIT(3)
#define NVQUIRK_ENABLE_SDR104		BIT(4)
#define NVQUIRK_ENABLE_DDR50		BIT(5)
#define NVQUIRK_HAS_PADCALIB		BIT(6)
#define NVQUIRK_HW_TAP_CONFIG		BIT(7)
#define NVQUIRK_DIS_CARD_CLK_CONFIG_TAP	BIT(8)
#define NVQUIRK_USE_PLATFORM_TUNING	BIT(9)
#define NVQUIRK_READ_REG_AFTER_WRITE	BIT(10)
#define NVQUIRK_SHADOW_XFER_MODE_WRITE	BIT(11)

#define MAX_CLK_PARENTS	5
#define MAX_DIVISOR_VALUE	128
#define MAX_TAP_VALUE	256
#define MAX_DQS_TRIM_VALUES	0x3F

#define SET_REQ_TAP	0x0
#define SET_DDR_TAP	0x1
#define SET_DEFAULT_TAP	0x2

const char *auto_calib_offset_prods[] = {
	"autocal-pu-pd-offset-default-3v3", /* DS */
	"autocal-pu-pd-offset-hs-3v3", /* MMC HS */
	"autocal-pu-pd-offset-hs-3v3", /* SD HS */
	"autocal-pu-pd-offset-default-1v8", /* SDR12 */
	"autocal-pu-pd-offset-hs-1v8", /* SDR25 */
	"autocal-pu-pd-offset-sdr50-1v8", /* SDR50 */
	"autocal-pu-pd-offset-sdr104-1v8", /* SDR104 */
	"autocal-pu-pd-offset-default-1v8", /* DDR50 */
	"autocal-pu-pd-offset-default-1v8", /* DDR52 */
	"autocal-pu-pd-offset-hs200-1v8", /* HS200 */
	"autocal-pu-pd-offset-hs400-1v8", /* HS400 */
};

static char prod_device_states[MMC_TIMING_COUNTER][20] = {
	"prod_c_ds", /* MMC_TIMING_LEGACY */
	"prod_c_hs", /* MMC_TIMING_MMC_HS */
	"prod_c_hs", /* MMC_TIMING_SD_HS */
	"prod_c_sdr12", /* MMC_TIMING_UHS_SDR12 */
	"prod_c_sdr25", /* MMC_TIMING_UHS_SDR25 */
	"prod_c_sdr50", /* MMC_TIMING_UHS_SDR50 */
	"prod_c_sdr104", /* MMC_TIMING_UHS_SDR104 */
	"prod_c_ddr52", /* MMC_TIMING_UHS_DDR50 */
	"prod_c_ddr52", /* MMC_TIMING_MMC_DDR52 */
	"prod_c_hs200", /* MMC_TIMING_MMC_HS200 */
	"prod_c_hs400", /* MMC_TIMING_MMC_HS400 */
};

struct sdhci_tegra_soc_data {
	const struct sdhci_pltfm_data *pdata;
	u32 nvquirks;
};

struct sdhci_tegra_clk_src_data {
	struct clk *parent_clk[MAX_CLK_PARENTS];
	const char *parent_clk_name[MAX_CLK_PARENTS];
	unsigned long parent_clk_rate[MAX_CLK_PARENTS];
	u8 parent_clk_src_cnt;
	u8 curr_parent_clk_idx;
};

struct sdhci_tegra {
	const struct sdhci_tegra_soc_data *soc_data;
	struct gpio_desc *power_gpio;
	struct reset_control *rst;
	bool ddr_signaling;
	bool pad_calib_required;
	struct sdhci_tegra_clk_src_data *clk_src_data;
	bool is_clk_enabled;
	unsigned long curr_clk_rate;
	unsigned long max_clk_limit;
	unsigned long max_ddr_clk_limit;
	struct tegra_prod *prods;
	u8 tuned_tap_delay;
	unsigned int tuning_status;
	#define TUNING_STATUS_DONE	1
	#define TUNING_STATUS_RETUNE	2
	bool disable_auto_cal;
	int dqs_trim_delay;
	int timing;
	bool set_1v8_calib_offsets;
	int current_voltage;
	struct padctrl *sdmmc_padctrl;
	unsigned int cd_irq;
	bool config_pad_ctrl;
	bool pwrdet_support;
	bool wake_enable_failed;
	bool cd_wakeup_capable;
	int cd_gpio;
	bool enable_hwcq;
};

/* Module params */
static unsigned int en_boot_part_access;

static void sdhci_tegra_debugfs_init(struct sdhci_host *host);
static void tegra_sdhci_vendor_trim_clear_sel_vreg(struct sdhci_host *host,
	bool enable);
static void tegra_sdhci_set_tap(struct sdhci_host *host, unsigned int tap,
	int type);
static int tegra_sdhci_suspend(struct sdhci_host *host);
static int tegra_sdhci_resume(struct sdhci_host *host);
static void tegra_sdhci_post_resume(struct sdhci_host *host);

static u16 tegra_sdhci_readw(struct sdhci_host *host, int reg)
{
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_tegra *tegra_host = sdhci_pltfm_priv(pltfm_host);
	const struct sdhci_tegra_soc_data *soc_data = tegra_host->soc_data;

	if (unlikely((soc_data->nvquirks & NVQUIRK_FORCE_SDHCI_SPEC_200) &&
			(reg == SDHCI_HOST_VERSION))) {
		/* Erratum: Version register is invalid in HW. */
		return SDHCI_SPEC_200;
	}

	return readw(host->ioaddr + reg);
}

static void tegra_sdhci_writeb(struct sdhci_host *host, u8 val, int reg)
{
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_tegra *tegra_host = sdhci_pltfm_priv(pltfm_host);
	const struct sdhci_tegra_soc_data *soc_data = tegra_host->soc_data;

	writeb(val, host->ioaddr + reg);
	if (soc_data->nvquirks & NVQUIRK_READ_REG_AFTER_WRITE)
		readb(host->ioaddr + reg);
}

static void tegra_sdhci_writew(struct sdhci_host *host, u16 val, int reg)
{
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_tegra *tegra_host = sdhci_pltfm_priv(pltfm_host);
	const struct sdhci_tegra_soc_data *soc_data = tegra_host->soc_data;

	if (soc_data->nvquirks & NVQUIRK_SHADOW_XFER_MODE_WRITE) {
		switch (reg) {
		case SDHCI_TRANSFER_MODE:
			/*
			 * Postpone this write, we must do it together with a
			 * command write that is down below.
			 */
			pltfm_host->xfer_mode_shadow = val;
			return;
		case SDHCI_COMMAND:
			writel((val << 16) | pltfm_host->xfer_mode_shadow,
				host->ioaddr + SDHCI_TRANSFER_MODE);
			if (soc_data->nvquirks & NVQUIRK_READ_REG_AFTER_WRITE)
				readl(host->ioaddr + SDHCI_TRANSFER_MODE);
			return;
		}
	}

	writew(val, host->ioaddr + reg);
	if (soc_data->nvquirks & NVQUIRK_READ_REG_AFTER_WRITE)
		readw(host->ioaddr + reg);
}

static void tegra_sdhci_writel(struct sdhci_host *host, u32 val, int reg)
{
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_tegra *tegra_host = sdhci_pltfm_priv(pltfm_host);
	const struct sdhci_tegra_soc_data *soc_data = tegra_host->soc_data;

	/* Seems like we're getting spurious timeout and crc errors, so
	 * disable signalling of them. In case of real errors software
	 * timers should take care of eventually detecting them.
	 */
	if (unlikely(reg == SDHCI_SIGNAL_ENABLE))
		val &= ~(SDHCI_INT_TIMEOUT|SDHCI_INT_CRC);

	writel(val, host->ioaddr + reg);
	if (soc_data->nvquirks & NVQUIRK_READ_REG_AFTER_WRITE)
		readl(host->ioaddr + reg);

	if (unlikely((soc_data->nvquirks & NVQUIRK_ENABLE_BLOCK_GAP_DET) &&
			(reg == SDHCI_INT_ENABLE))) {
		/* Erratum: Must enable block gap interrupt detection */
		u8 gap_ctrl = readb(host->ioaddr + SDHCI_BLOCK_GAP_CONTROL);
		if (val & SDHCI_INT_CARD_INT)
			gap_ctrl |= 0x8;
		else
			gap_ctrl &= ~0x8;
		writeb(gap_ctrl, host->ioaddr + SDHCI_BLOCK_GAP_CONTROL);
		if (soc_data->nvquirks & NVQUIRK_READ_REG_AFTER_WRITE)
			readb(host->ioaddr + SDHCI_BLOCK_GAP_CONTROL);
	}
}

static void tegra_sdhci_dump_vendor_regs(struct sdhci_host *host)
{
	int reg, tuning_status;
	u8 i;

	pr_err("======= %s: Tuning windows =======\n", mmc_hostname(host->mmc));
	reg = sdhci_readl(host, SDHCI_VNDR_TUN_CTRL0_0);
	for (i = 0; i <= TUNING_WORD_SEL_MASK; i++) {
		reg &= ~SDHCI_TUN_CTRL0_TUNING_WORD_SEL_MASK;
		reg |= i;
		sdhci_writel(host, reg, SDHCI_VNDR_TUN_CTRL0_0);
		tuning_status = sdhci_readl(host, SDHCI_TEGRA_VNDR_TUNING_STATUS0);
		pr_info("%s: tuning window[%d]: %#x\n",
			mmc_hostname(host->mmc), i, tuning_status);
	}
	pr_err("==================================\n");

	pr_err("Vendor clock ctrl: %#x\n",
		sdhci_readl(host, SDHCI_TEGRA_VENDOR_CLOCK_CTRL));
	pr_err("Vendor SysSW ctrl: %#x\n",
		sdhci_readl(host, SDHCI_TEGRA_VENDOR_SYS_SW_CTRL));
	pr_err("Vendor Err interrupt status : %#x\n",
		sdhci_readl(host, SDHCI_TEGRA_VENDOR_ERR_INTR_STATUS));
	pr_err("Vendor Cap overrides: %#x\n",
		sdhci_readl(host, SDHCI_TEGRA_VENDOR_CAP_OVERRIDES));
	pr_err("Vendor Misc ctrl: %#x\n",
		sdhci_readl(host, SDHCI_TEGRA_VENDOR_MISC_CTRL));
	pr_err("Vendor Misc ctrl_1: %#x\n",
		sdhci_readl(host, SDHCI_TEGRA_VENDOR_MISC_CTRL_1));
	pr_err("Vendor Misc ctrl_2: %#x\n",
		sdhci_readl(host, SDHCI_TEGRA_VENDOR_MISC_CTRL_2));
	pr_err("Vendor IO trim ctrl: %#x\n",
		sdhci_readl(host, SDMMC_VNDR_IO_TRIM_CTRL_0));
	pr_err("Vendor Tuning ctrl: %#x\n",
		sdhci_readl(host, SDHCI_VNDR_TUN_CTRL0_0));
	pr_err("SDMEM comp padctrl: %#x\n",
		sdhci_readl(host, SDHCI_TEGRA_SDMEM_COMP_PADCTRL));
	pr_err("Autocal config: %#x\n",
		sdhci_readl(host, SDHCI_TEGRA_AUTO_CAL_CONFIG));
	pr_err("Autocal status: %#x\n",
		sdhci_readl(host, SDHCI_TEGRA_AUTO_CAL_STATUS));
}

static void tegra_sdhci_card_event(struct sdhci_host *host)
{
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_tegra *tegra_host = sdhci_pltfm_priv(pltfm_host);
	int present = mmc_gpio_get_cd(host->mmc);

	if (!present) {
		tegra_host->tuning_status = TUNING_STATUS_RETUNE;
	} else {
		tegra_host->set_1v8_calib_offsets = false;
	}
}

static unsigned int tegra_sdhci_get_ro(struct sdhci_host *host)
{
	return mmc_gpio_get_ro(host->mmc);
}

static void tegra_sdhci_post_init(struct sdhci_host *host)
{
	int reg, timeout = 5;

	reg = sdhci_readl(host, SDHCI_TEGRA_VENDOR_DLLCAL_CFG);
	reg |= SDHCI_DLLCAL_CFG_EN_CALIBRATE;
	sdhci_writel(host, reg, SDHCI_TEGRA_VENDOR_DLLCAL_CFG);

	mdelay(1);

	/* Wait until DLL calibration is done */
	do {
		if (!(sdhci_readl(host, SDHCI_DLLCAL_CFG_STATUS) &
			SDHCI_DLLCAL_CFG_STATUS_DLL_ACTIVE))
			break;
		mdelay(1);
		timeout--;
	} while (timeout);

	if (!timeout)
		dev_err(mmc_dev(host->mmc), "DLL calibration timed out\n");
}

static void tegra_sdhci_hs400_enhanced_strobe(struct sdhci_host *host, bool enable)
{
	int reg;

	reg = sdhci_readl(host, SDHCI_TEGRA_VENDOR_SYS_SW_CTRL);
	if (enable)
		reg |= SDHCI_SYS_SW_CTRL_STROBE_EN;
	else
		reg &= ~SDHCI_SYS_SW_CTRL_STROBE_EN;
	sdhci_writel(host, reg, SDHCI_TEGRA_VENDOR_SYS_SW_CTRL);
}

static int tegra_sdhci_get_max_tuning_loop_counter(struct sdhci_host *host)
{
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_tegra *tegra_host = sdhci_pltfm_priv(pltfm_host);
	int err = 0;

	err = tegra_prod_set_by_name_partially(&host->ioaddr,
			prod_device_states[host->mmc->ios.timing],
			tegra_host->prods, 0, SDHCI_VNDR_TUN_CTRL0_0,
			SDHCI_VNDR_TUN_CTRL0_0_TUN_ITER_MASK);
	if (err)
		dev_err(mmc_dev(host->mmc),
			"%s: error %d in tuning iteration update\n",
				__func__, err);

	return 257;
}

static bool tegra_sdhci_skip_retuning(struct sdhci_host *host)
{
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_tegra *tegra_host = sdhci_pltfm_priv(pltfm_host);

	if (tegra_host->tuning_status == TUNING_STATUS_DONE) {
		dev_info(mmc_dev(host->mmc),
			"Tuning done, restoring the best tap value : %u\n",
				tegra_host->tuned_tap_delay);
		tegra_sdhci_set_tap(host, tegra_host->tuned_tap_delay,
			SET_REQ_TAP);
		pr_err("%s: %s: returning true\n", mmc_hostname(host->mmc), __func__);
		return true;
	}

	return false;
}

static void tegra_sdhci_post_tuning(struct sdhci_host *host)
{
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_tegra *tegra_host = sdhci_pltfm_priv(pltfm_host);
	u32 reg;

	reg = sdhci_readl(host, SDHCI_TEGRA_VENDOR_CLOCK_CTRL);
	tegra_host->tuned_tap_delay = ((reg & SDHCI_CLOCK_CTRL_TAP_MASK) >>
		SDHCI_CLOCK_CTRL_TAP_SHIFT);
	tegra_host->tuning_status = TUNING_STATUS_DONE;
}

static void tegra_sdhci_vendor_trim_clear_sel_vreg(struct sdhci_host *host,
	bool enable)
{
	unsigned int misc_ctrl;

	misc_ctrl = sdhci_readl(host, SDMMC_VNDR_IO_TRIM_CTRL_0);
	if (enable) {
		misc_ctrl &= ~(SDMMC_VNDR_IO_TRIM_CTRL_0_SEL_VREG_MASK);
		sdhci_writel(host, misc_ctrl, SDMMC_VNDR_IO_TRIM_CTRL_0);
		udelay(3);
		sdhci_reset(host, SDHCI_RESET_CMD | SDHCI_RESET_DATA);
	} else {
		misc_ctrl |= (SDMMC_VNDR_IO_TRIM_CTRL_0_SEL_VREG_MASK);
		sdhci_writel(host, misc_ctrl, SDMMC_VNDR_IO_TRIM_CTRL_0);
		udelay(1);
	}
}

static void tegra_sdhci_reset(struct sdhci_host *host, u8 mask)
{
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_tegra *tegra_host = sdhci_pltfm_priv(pltfm_host);
	const struct sdhci_tegra_soc_data *soc_data = tegra_host->soc_data;
	u32 misc_ctrl, clk_ctrl;
	int err;

	sdhci_reset(host, mask);

	if (!(mask & SDHCI_RESET_ALL))
		return;

	err = tegra_prod_set_by_name(&host->ioaddr, "prod",
		tegra_host->prods);
	if (err)
		dev_err(mmc_dev(host->mmc),
			"Failed to set prod-reset settings %d\n", err);

	/* Set the tap delay value */
	if (!tegra_sdhci_skip_retuning(host))
		tegra_sdhci_set_tap(host, 0, SET_DEFAULT_TAP);

	misc_ctrl = sdhci_readl(host, SDHCI_TEGRA_VENDOR_MISC_CTRL);
	clk_ctrl = sdhci_readl(host, SDHCI_TEGRA_VENDOR_CLOCK_CTRL);
	misc_ctrl &= ~(SDHCI_MISC_CTRL_ENABLE_SDHCI_SPEC_300 |
		       SDHCI_MISC_CTRL_ENABLE_SDR50 |
		       SDHCI_MISC_CTRL_ENABLE_DDR50 |
		       SDHCI_MISC_CTRL_ENABLE_SDR104);

	/*
	 * If the board does not define a regulator for the SDHCI
	 * IO voltage, then don't advertise support for UHS modes
	 * even if the device supports it because the IO voltage
	 * cannot be configured.
	 */
	if (!IS_ERR(host->mmc->supply.vqmmc)) {
		/* Erratum: Enable SDHCI spec v3.00 support */
		if (soc_data->nvquirks & NVQUIRK_ENABLE_SDHCI_SPEC_300)
			misc_ctrl |= SDHCI_MISC_CTRL_ENABLE_SDHCI_SPEC_300;
		/* Advertise UHS modes as supported by host */
		if (soc_data->nvquirks & NVQUIRK_ENABLE_SDR50)
			misc_ctrl |= SDHCI_MISC_CTRL_ENABLE_SDR50;
		if (soc_data->nvquirks & NVQUIRK_ENABLE_DDR50)
			misc_ctrl |= SDHCI_MISC_CTRL_ENABLE_DDR50;
		if (soc_data->nvquirks & NVQUIRK_ENABLE_SDR104)
			misc_ctrl |= SDHCI_MISC_CTRL_ENABLE_SDR104;
		if (soc_data->nvquirks & NVQUIRK_ENABLE_SDR50)
			clk_ctrl |= SDHCI_CLOCK_CTRL_SDR50_TUNING_OVERRIDE;
	}

	sdhci_writel(host, misc_ctrl, SDHCI_TEGRA_VENDOR_MISC_CTRL);
	sdhci_writel(host, clk_ctrl, SDHCI_TEGRA_VENDOR_CLOCK_CTRL);

	/* SEL_VREG should be 0 for all modes*/
	tegra_sdhci_vendor_trim_clear_sel_vreg(host, true);

	if (soc_data->nvquirks & NVQUIRK_HAS_PADCALIB)
		tegra_host->pad_calib_required = true;

	tegra_host->ddr_signaling = false;
}

static void tegra_sdhci_set_bus_width(struct sdhci_host *host, int bus_width)
{
	u32 ctrl;

	ctrl = sdhci_readb(host, SDHCI_HOST_CONTROL);
	if ((host->mmc->caps & MMC_CAP_8_BIT_DATA) &&
	    (bus_width == MMC_BUS_WIDTH_8)) {
		ctrl &= ~SDHCI_CTRL_4BITBUS;
		ctrl |= SDHCI_CTRL_8BITBUS;
	} else {
		ctrl &= ~SDHCI_CTRL_8BITBUS;
		if (bus_width == MMC_BUS_WIDTH_4)
			ctrl |= SDHCI_CTRL_4BITBUS;
		else
			ctrl &= ~SDHCI_CTRL_4BITBUS;
	}
	sdhci_writeb(host, ctrl, SDHCI_HOST_CONTROL);
}

static void tegra_sdhci_configure_e_input(struct sdhci_host *host, bool enable)
{
	u32 reg;

	reg = sdhci_readl(host, SDHCI_TEGRA_SDMEM_COMP_PADCTRL);
	if (enable)
		reg |= SDHCI_TEGRA_PAD_E_INPUT_OR_E_PWRD_MASK;
	else
		reg &= ~SDHCI_TEGRA_PAD_E_INPUT_OR_E_PWRD_MASK;
	sdhci_writel(host, reg, SDHCI_TEGRA_SDMEM_COMP_PADCTRL);
	udelay(1);
}

static void tegra_sdhci_pad_autocalib(struct sdhci_host *host)
{
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_tegra *tegra_host = sdhci_pltfm_priv(pltfm_host);
	u32 val, signal_voltage;
	u16 clk;
	int card_clk_enabled, ret;
	const char *comp_vref;
	unsigned int timeout = 10;
	unsigned int timing = host->mmc->ios.timing;

	if (tegra_host->disable_auto_cal)
		return;

	signal_voltage = host->mmc->ios.signal_voltage;
	comp_vref = (signal_voltage == MMC_SIGNAL_VOLTAGE_330) ?
		"comp-vref-3v3" : "comp-vref-1v8";

	clk = sdhci_readw(host, SDHCI_CLOCK_CONTROL);
	card_clk_enabled = clk & SDHCI_CLOCK_CARD_EN;

	if (card_clk_enabled) {
		clk &= ~SDHCI_CLOCK_CARD_EN;
		sdhci_writew(host, clk, SDHCI_CLOCK_CONTROL);
	}

	tegra_sdhci_configure_e_input(host, true);
	udelay(1);

	ret = tegra_prod_set_by_name_partially(&host->ioaddr,
			prod_device_states[timing],
			tegra_host->prods, 0, SDHCI_TEGRA_SDMEM_COMP_PADCTRL,
			SDHCI_TEGRA_SDMEMCOMP_PADCTRL_VREF_SEL);
	if (ret < 0)
		dev_err(mmc_dev(host->mmc),
			"%s: error %d in comp vref settings\n",
			__func__, ret);

	/* Enable Auto Calibration*/
	ret = tegra_prod_set_by_name_partially(&host->ioaddr,
			prod_device_states[timing],
			tegra_host->prods, 0, SDHCI_TEGRA_AUTO_CAL_CONFIG,
			SDHCI_AUTO_CAL_ENABLE);
	if (ret < 0)
		dev_err(mmc_dev(host->mmc),
			"%s: error %d in autocal-en settings\n",
			__func__, ret);

	val = sdhci_readl(host, SDHCI_TEGRA_AUTO_CAL_CONFIG);
	val |= SDHCI_AUTO_CAL_START;
	sdhci_writel(host,val, SDHCI_TEGRA_AUTO_CAL_CONFIG);

	/* Program calibration offsets */
	ret = tegra_prod_set_by_name_partially(&host->ioaddr,
			prod_device_states[timing],
			tegra_host->prods, 0, SDHCI_TEGRA_AUTO_CAL_CONFIG,
			SDHCI_AUTO_CAL_PUPD_OFFSETS);
	if (ret < 0)
		dev_err(mmc_dev(host->mmc),
			"error %d in autocal-pu-pd-offset settings\n", ret);

	/* Wait 2us after auto calibration is enabled */
	udelay(2);

	/* Wait until calibration is done */
	do {
		if (!(sdhci_readl(host, SDHCI_TEGRA_AUTO_CAL_STATUS) &
				SDHCI_TEGRA_AUTO_CAL_ACTIVE))
			break;
		mdelay(1);
		timeout--;
	} while (timeout);

	if (!timeout)
		dev_err(mmc_dev(host->mmc), "Auto calibration timed out\n");

	tegra_sdhci_configure_e_input(host, false);

	if (card_clk_enabled) {
		clk |= SDHCI_CLOCK_CARD_EN;
		sdhci_writew(host, clk, SDHCI_CLOCK_CONTROL);
	}
}

static unsigned long get_nearest_clock_freq(unsigned long parent_rate,
	unsigned long desired_rate)
{
	unsigned long result, result_frac_div;
	int div, rem;

	if (parent_rate <= desired_rate)
		return parent_rate;

	div = parent_rate / desired_rate;
	div = (div > MAX_DIVISOR_VALUE) ? MAX_DIVISOR_VALUE : div;
	rem = parent_rate % desired_rate;
	result = parent_rate / div;
	if (div == MAX_DIVISOR_VALUE || !rem)
		return (parent_rate / div);
	else if (result > desired_rate) {
		result_frac_div = (parent_rate << 1) / ((div << 1) + 1);
		if (result_frac_div > desired_rate)
			return (parent_rate / (div + 1));
		else
			return result_frac_div;
	}

	return result;
}

static void tegra_sdhci_set_clk_parent(struct sdhci_host *host,
	unsigned long desired_rate)
{
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_tegra *tegra_host = sdhci_pltfm_priv(pltfm_host);
	struct sdhci_tegra_clk_src_data *clk_src_data;
	unsigned long parent_clk_rate, rate, nearest_freq_rate = 0;
	int rc;
	u8 i, sel_parent_idx = 0;

	if (tegra_platform_is_fpga())
		return;

	clk_src_data = tegra_host->clk_src_data;
	if (!clk_src_data) {
		dev_err(mmc_dev(host->mmc), "clk src data NULL");
		return;
	}

	for (i = 0; i < clk_src_data->parent_clk_src_cnt; i++) {
		parent_clk_rate = clk_src_data->parent_clk_rate[i];
		rate = get_nearest_clock_freq(parent_clk_rate, desired_rate);
		if (rate > nearest_freq_rate) {
			nearest_freq_rate = rate;
			sel_parent_idx = i;
		}
	}

	dev_dbg(mmc_dev(host->mmc), "chosen clk parent %s, parent rate %lu\n",
		clk_src_data->parent_clk_name[sel_parent_idx],
		clk_src_data->parent_clk_rate[sel_parent_idx]);
	/* Do nothing if the desired parent is already set */
	if (clk_src_data->curr_parent_clk_idx == sel_parent_idx)
		return;
	else {
		rc = clk_set_parent(pltfm_host->clk,
			clk_src_data->parent_clk[sel_parent_idx]);
		if (rc)
			dev_err(mmc_dev(host->mmc),
				"Failed to set parent pll %d\n", rc);
		else
			clk_src_data->curr_parent_clk_idx = sel_parent_idx;
	}
}

static void tegra_sdhci_set_clk_rate(struct sdhci_host *host,
	unsigned long host_clk)
{
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_tegra *tegra_host = sdhci_pltfm_priv(pltfm_host);
	int rc;

	if (host_clk == tegra_host->curr_clk_rate)
		return;

	/* Set the required clock parent based on the desired rate */
	tegra_sdhci_set_clk_parent(host, host_clk);

	/*
	 * Proceed irrespective of parent selection as the interface could
	 * work at a lower frequency too. Parent clk selection would report
	 * errors in the logs.
	 */
	rc = clk_set_rate(pltfm_host->clk, host_clk);
	if (rc)
		dev_err(mmc_dev(host->mmc),
			"Failed to set %lu clk rate\n", host_clk);
	else {
		/*
		 * Clock frequency actually set would be slightly different from
		 * desired rate. Next request would again come for the desired
		 * rate. Hence, store the desired rate in curr_clk_rate.
		 */
		tegra_host->curr_clk_rate = host_clk;
	}
}

static unsigned long tegra_sdhci_apply_clk_limits(struct sdhci_host *host,
	unsigned int clock)
{
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_tegra *tegra_host = sdhci_pltfm_priv(pltfm_host);
	unsigned long host_clk;

	if (tegra_host->ddr_signaling)
		host_clk = (tegra_host->max_ddr_clk_limit) ?
			tegra_host->max_ddr_clk_limit * 2 : clock * 2;
	else
		host_clk = (clock > tegra_host->max_clk_limit) ?
			tegra_host->max_clk_limit : clock;

	dev_dbg(mmc_dev(host->mmc), "Setting clk limit %lu\n", host_clk);
	return host_clk;
}

static void tegra_sdhci_set_clock(struct sdhci_host *host, unsigned int clock)
{
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_tegra *tegra_host = sdhci_pltfm_priv(pltfm_host);
	unsigned long host_clk;
	int rc;
	u8 vndr_ctrl;

	if (tegra_platform_is_vdk())
		return;

	host_clk = tegra_sdhci_apply_clk_limits(host, clock);

	if (clock) {
		/* Enable SDMMC host CAR clock */
		if (!tegra_host->is_clk_enabled) {
			rc = clk_prepare_enable(pltfm_host->clk);
			if (rc) {
				dev_err(mmc_dev(host->mmc),
					"clk enable failed %d\n", rc);
				return;
			}
			tegra_host->is_clk_enabled = true;
			vndr_ctrl = sdhci_readb(host,
				SDHCI_TEGRA_VENDOR_CLOCK_CTRL);
			vndr_ctrl |= SDHCI_CLOCK_CTRL_SDMMC_CLK;
			sdhci_writeb(host, vndr_ctrl,
				SDHCI_TEGRA_VENDOR_CLOCK_CTRL);
			/* power up / active state */
			tegra_sdhci_vendor_trim_clear_sel_vreg(host, true);
		}

		/* Set the desired clk freq rate */
		tegra_sdhci_set_clk_rate(host, host_clk);
		host->max_clk = clk_get_rate(pltfm_host->clk);
		dev_dbg(mmc_dev(host->mmc), "req clk %lu, set clk %d\n",
			host_clk, host->max_clk);
		/* Run auto calibration if required */
		if (tegra_host->pad_calib_required) {
			tegra_sdhci_pad_autocalib(host);
			tegra_host->pad_calib_required = false;
		}

		/* Enable SDMMC internal and card clocks */
		sdhci_set_clock(host, clock);
	} else {
		/* Disable the card and internal clocks first */
		sdhci_set_clock(host, clock);

		/* Disable SDMMC host CAR clock */
		if (tegra_host->is_clk_enabled) {
			/* power down / idle state */
			tegra_sdhci_vendor_trim_clear_sel_vreg(host, false);

			vndr_ctrl = sdhci_readb(host,
				SDHCI_TEGRA_VENDOR_CLOCK_CTRL);
			vndr_ctrl &= ~SDHCI_CLOCK_CTRL_SDMMC_CLK;
			sdhci_writeb(host, vndr_ctrl,
				SDHCI_TEGRA_VENDOR_CLOCK_CTRL);
			clk_disable_unprepare(pltfm_host->clk);
			tegra_host->is_clk_enabled = false;
		}

	}
}

static inline int tegra_sdhci_set_dqs_trim_delay(struct sdhci_host *host,
	int dqs_trim_delay)
{
	u32 reg;

	if ((dqs_trim_delay > MAX_DQS_TRIM_VALUES) || (dqs_trim_delay < 0)) {
		dev_err(mmc_dev(host->mmc), "Invalid dqs trim value\n");
		return -1;
	}

	reg = sdhci_readl(host, SDHCI_TEGRA_VENDOR_CAP_OVERRIDES);
	reg &= ~(SDHCI_VENDOR_CAP_DQS_TRIM_MASK <<
		SDHCI_VENDOR_CAP_DQS_TRIM_SHIFT);
	reg |= (dqs_trim_delay << SDHCI_VENDOR_CAP_DQS_TRIM_SHIFT);
	sdhci_writel(host, reg, SDHCI_TEGRA_VENDOR_CAP_OVERRIDES);

	return 0;
}

static void tegra_sdhci_set_uhs_signaling(struct sdhci_host *host,
					  unsigned timing)
{
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_tegra *tegra_host = sdhci_pltfm_priv(pltfm_host);
	int ret;
	u8 tap_delay_type;
	bool tuning_mode = false;

	if ((timing == MMC_TIMING_UHS_DDR50) ||
		(timing == MMC_TIMING_MMC_DDR52))
		tegra_host->ddr_signaling = true;

	if ((timing == MMC_TIMING_UHS_SDR104) ||
		(timing == MMC_TIMING_UHS_SDR50) ||
		(timing == MMC_TIMING_MMC_HS200) ||
		(timing == MMC_TIMING_MMC_HS400))
		tuning_mode = true;

	sdhci_set_uhs_signaling(host, timing);

	/* Set DQS trim delay */
	if (timing == MMC_TIMING_MMC_HS400) {
		tegra_sdhci_set_dqs_trim_delay(host,
			tegra_host->dqs_trim_delay);
	}

	/* Set trim delay */
	if (tegra_host->ddr_signaling || (timing == MMC_TIMING_MMC_HS200)) {
		ret = tegra_prod_set_by_name_partially(&host->ioaddr,
				prod_device_states[timing], tegra_host->prods,
				0, SDHCI_TEGRA_VENDOR_CLOCK_CTRL,
				SDHCI_CLOCK_CTRL_TRIM_MASK <<
				SDHCI_CLOCK_CTRL_TRIM_SHIFT);
		if (ret < 0)
			dev_err(mmc_dev(host->mmc),
				"Failed to set trim value for timing %d, %d\n",
				timing, ret);
	}

	/* Set Tap delay */
	if (tegra_host->ddr_signaling)
		tap_delay_type = SET_DDR_TAP;
	else if ((tegra_host->tuning_status == TUNING_STATUS_DONE) &&
		tuning_mode)
		tap_delay_type = SET_REQ_TAP;
	else
		tap_delay_type = SET_DEFAULT_TAP;
	tegra_sdhci_set_tap(host, tegra_host->tuned_tap_delay, tap_delay_type);

	switch (timing) {
	case MMC_TIMING_UHS_SDR12:
	case MMC_TIMING_UHS_SDR25:
	case MMC_TIMING_UHS_DDR50:
	case MMC_TIMING_UHS_SDR104:
	case MMC_TIMING_MMC_DDR52:
	case MMC_TIMING_MMC_HS200:
	case MMC_TIMING_MMC_HS400:
		if ((timing > tegra_host->timing) &&
				!tegra_host->set_1v8_calib_offsets) {
			tegra_sdhci_pad_autocalib(host);
			tegra_host->set_1v8_calib_offsets = true;
			tegra_host->timing = timing;
		}
	}
}

static unsigned int tegra_sdhci_get_max_clock(struct sdhci_host *host)
{
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);

	/*
	 * DDR modes require the host to run at double the card frequency, so
	 * the maximum rate we can support is half of the module input clock.
	 */
	return clk_round_rate(pltfm_host->clk, UINT_MAX) / 2;
}

static void tegra_sdhci_set_tap(struct sdhci_host *host, unsigned int tap,
	int type)
{
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_tegra *tegra_host = sdhci_pltfm_priv(pltfm_host);
	const struct sdhci_tegra_soc_data *soc_data = tegra_host->soc_data;
	u32 reg;
	u16 clk;
	bool card_clk_enabled = false;
	int err;

	if ((tap < 0)  || (tap > MAX_TAP_VALUE)) {
		dev_err(mmc_dev(host->mmc), "Invalid tap value %d\n", tap);
		return;
	}

	if (soc_data->nvquirks & NVQUIRK_DIS_CARD_CLK_CONFIG_TAP) {
		clk = sdhci_readw(host, SDHCI_CLOCK_CONTROL);
		card_clk_enabled = clk & SDHCI_CLOCK_CARD_EN;
		if (card_clk_enabled) {
			clk &= ~SDHCI_CLOCK_CARD_EN;
			sdhci_writew(host, clk, SDHCI_CLOCK_CONTROL);
		}
	}

	/* Disable HW tap delay config */
	if (soc_data->nvquirks & NVQUIRK_HW_TAP_CONFIG) {
		reg = sdhci_readl(host, SDHCI_VNDR_TUN_CTRL0_0);
		reg &= ~SDHCI_VNDR_TUN_CTRL0_TUN_HW_TAP;
		sdhci_writel(host, reg, SDHCI_VNDR_TUN_CTRL0_0);
	}

	if (type & (SET_DDR_TAP | SET_DEFAULT_TAP)) {
		err = tegra_prod_set_by_name_partially(&host->ioaddr,
				prod_device_states[host->mmc->ios.timing],
				tegra_host->prods, 0,
				SDHCI_TEGRA_VENDOR_CLOCK_CTRL,
				SDHCI_CLOCK_CTRL_TAP_MASK <<
				SDHCI_CLOCK_CTRL_TAP_SHIFT);
		if (err < 0)
			dev_err(mmc_dev(host->mmc),
				"%s: error %d in tap settings, timing: %d\n",
				__func__, err, host->mmc->ios.timing);
	} else {
		reg = sdhci_readl(host, SDHCI_TEGRA_VENDOR_CLOCK_CTRL);
		reg &= ~SDHCI_CLOCK_CTRL_TAP_MASK;
		reg |= tap << SDHCI_CLOCK_CTRL_TAP_SHIFT;
		sdhci_writel(host, reg, SDHCI_TEGRA_VENDOR_CLOCK_CTRL);
	}

	/* Enable HW tap delay config */
	if (soc_data->nvquirks & NVQUIRK_HW_TAP_CONFIG) {
		reg = sdhci_readl(host, SDHCI_VNDR_TUN_CTRL0_0);
		reg |= SDHCI_VNDR_TUN_CTRL0_TUN_HW_TAP;
		sdhci_writel(host, reg, SDHCI_VNDR_TUN_CTRL0_0);
	}

	if ((soc_data->nvquirks & NVQUIRK_DIS_CARD_CLK_CONFIG_TAP) &&
		card_clk_enabled) {
		udelay(1);
		sdhci_reset(host, SDHCI_RESET_CMD | SDHCI_RESET_DATA);
		clk = sdhci_readw(host, SDHCI_CLOCK_CONTROL);
		clk |= SDHCI_CLOCK_CARD_EN;
		sdhci_writew(host, clk, SDHCI_CLOCK_CONTROL);
	}
}

static int tegra_sdhci_execute_tuning(struct sdhci_host *host, u32 opcode)
{
	unsigned int min, max;

	/*
	 * Start search for minimum tap value at 10, as smaller values are
	 * may wrongly be reported as working but fail at higher speeds,
	 * according to the TRM.
	 */
	min = 10;
	while (min < 255) {
		tegra_sdhci_set_tap(host, min, SET_REQ_TAP);
		if (!mmc_send_tuning(host->mmc, opcode, NULL))
			break;
		min++;
	}

	/* Find the maximum tap value that still passes. */
	max = min + 1;
	while (max < 255) {
		tegra_sdhci_set_tap(host, max, SET_REQ_TAP);
		if (mmc_send_tuning(host->mmc, opcode, NULL)) {
			max--;
			break;
		}
		max++;
	}

	/* The TRM states the ideal tap value is at 75% in the passing range. */
	tegra_sdhci_set_tap(host, min + ((max - min) * 3 / 4), SET_REQ_TAP);

	return mmc_send_tuning(host->mmc, opcode, NULL);
}

static void tegra_sdhci_set_padctrl(struct sdhci_host *host, int voltage)
{
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_tegra *tegra_host = sdhci_pltfm_priv(pltfm_host);
	int ret;

	if (tegra_host->pwrdet_support && tegra_host->sdmmc_padctrl) {
		ret = padctrl_set_voltage(tegra_host->sdmmc_padctrl, voltage);
		if (ret)
			dev_err(mmc_dev(host->mmc),
				"Failed to set sdmmc padctrl %d\n", ret);
	}
}

static void tegra_sdhci_signal_voltage_switch_pre(struct sdhci_host *host,
	int signal_voltage)
{
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_tegra *tegra_host = sdhci_pltfm_priv(pltfm_host);

	if (IS_ERR_OR_NULL(host->mmc->supply.vqmmc)) {
		dev_err(mmc_dev(host->mmc), "vqmmc supply missing\n");
		return;
	}

	tegra_host->current_voltage =
		regulator_get_voltage(host->mmc->supply.vqmmc);

	/* For 3.3V, pwrdet should be set before setting the voltage */
	if (signal_voltage == MMC_SIGNAL_VOLTAGE_330) {
		if (tegra_host->current_voltage < 2700000)
			tegra_sdhci_set_padctrl(host, 3300000);
	}
	tegra_host->config_pad_ctrl = true;
}

static void tegra_sdhci_signal_voltage_switch_post(struct sdhci_host *host,
	int signal_voltage)
{
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_tegra *tegra_host = sdhci_pltfm_priv(pltfm_host);
	int voltage;
	bool set;

	if (IS_ERR_OR_NULL(host->mmc->supply.vqmmc)) {
		dev_err(mmc_dev(host->mmc), "vqmmc supply missing\n");
		return;
	}

	set = (signal_voltage == MMC_SIGNAL_VOLTAGE_180) ? true : false;
	if (tegra_host->config_pad_ctrl) {
		voltage = regulator_get_voltage(host->mmc->supply.vqmmc);
		if ((voltage < tegra_host->current_voltage) && set)
			tegra_sdhci_set_padctrl(host, 1800000);
	}

	if (tegra_host->pad_calib_required)
		tegra_sdhci_pad_autocalib(host);
}

static void tegra_sdhci_voltage_switch(struct sdhci_host *host)
{
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_tegra *tegra_host = sdhci_pltfm_priv(pltfm_host);
	const struct sdhci_tegra_soc_data *soc_data = tegra_host->soc_data;

	if (soc_data->nvquirks & NVQUIRK_HAS_PADCALIB)
		tegra_host->pad_calib_required = true;
}

static int sdhci_tegra_get_parent_pll_from_dt(struct sdhci_host *host,
	struct platform_device *pdev)
{
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_tegra *tegra_host = sdhci_pltfm_priv(pltfm_host);
	struct device_node *np = pdev->dev.of_node;
	struct sdhci_tegra_clk_src_data *clk_src_data;
	struct clk *parent_clk;
	const char *pll_str;
	int i, cnt, j = 0;

	if (!np || !tegra_host)
		return -EINVAL;

	if (!of_find_property(np, "pll_source", NULL))
		return -ENXIO;

	clk_src_data = tegra_host->clk_src_data;
	cnt = of_property_count_strings(np, "pll_source");
	if (!cnt)
		return -EINVAL;

	if (cnt > MAX_CLK_PARENTS) {
		dev_warn(mmc_dev(host->mmc),
			"Parent sources list exceeded limit\n");
		cnt = MAX_CLK_PARENTS;
	}

	for (i = 0; i < cnt; i++) {
		of_property_read_string_index(np, "pll_source", i, &pll_str);
		parent_clk = devm_clk_get(&pdev->dev, pll_str);
		if (IS_ERR(parent_clk))
			dev_err(mmc_dev(host->mmc), "Failed to get %s clk\n",
				pll_str);
		else {
			clk_src_data->parent_clk_name[j] = pll_str;
			clk_src_data->parent_clk_rate[j] = clk_get_rate(parent_clk);
			clk_src_data->parent_clk[j++] = parent_clk;
		}
	}

	/* Count valid parent clock sources with clk structures */
	clk_src_data->parent_clk_src_cnt = j;

	return 0;
}

static const struct sdhci_ops tegra_sdhci_ops = {
	.get_ro     = tegra_sdhci_get_ro,
	.read_w     = tegra_sdhci_readw,
	.write_b    = tegra_sdhci_writeb,
	.write_w    = tegra_sdhci_writew,
	.write_l    = tegra_sdhci_writel,
	.set_clock  = tegra_sdhci_set_clock,
	.set_bus_width = tegra_sdhci_set_bus_width,
	.reset      = tegra_sdhci_reset,
	.set_uhs_signaling = tegra_sdhci_set_uhs_signaling,
	.voltage_switch = tegra_sdhci_voltage_switch,
	.get_max_clock = tegra_sdhci_get_max_clock,
	.get_max_tuning_loop_counter = tegra_sdhci_get_max_tuning_loop_counter,
	.skip_retuning = tegra_sdhci_skip_retuning,
	.post_tuning = tegra_sdhci_post_tuning,
	.voltage_switch_pre = tegra_sdhci_signal_voltage_switch_pre,
	.voltage_switch_post = tegra_sdhci_signal_voltage_switch_post,
	.hs400_enhanced_strobe = tegra_sdhci_hs400_enhanced_strobe,
	.post_init = tegra_sdhci_post_init,
	.suspend = tegra_sdhci_suspend,
	.resume = tegra_sdhci_resume,
	.platform_resume = tegra_sdhci_post_resume,
	.card_event = tegra_sdhci_card_event,
	.dump_vendor_regs = tegra_sdhci_dump_vendor_regs,
};

static const struct sdhci_pltfm_data sdhci_tegra20_pdata = {
	.quirks = SDHCI_QUIRK_BROKEN_TIMEOUT_VAL |
		  SDHCI_QUIRK_SINGLE_POWER_WRITE |
		  SDHCI_QUIRK_NO_HISPD_BIT |
		  SDHCI_QUIRK_BROKEN_ADMA_ZEROLEN_DESC |
		  SDHCI_QUIRK_CAP_CLOCK_BASE_BROKEN,
	.ops  = &tegra_sdhci_ops,
};

static const struct sdhci_tegra_soc_data soc_data_tegra20 = {
	.pdata = &sdhci_tegra20_pdata,
	.nvquirks = NVQUIRK_FORCE_SDHCI_SPEC_200 |
		    NVQUIRK_ENABLE_BLOCK_GAP_DET,
};

static const struct sdhci_pltfm_data sdhci_tegra30_pdata = {
	.quirks = SDHCI_QUIRK_BROKEN_TIMEOUT_VAL |
		  SDHCI_QUIRK_DATA_TIMEOUT_USES_SDCLK |
		  SDHCI_QUIRK_SINGLE_POWER_WRITE |
		  SDHCI_QUIRK_NO_HISPD_BIT |
		  SDHCI_QUIRK_BROKEN_ADMA_ZEROLEN_DESC |
		  SDHCI_QUIRK_CAP_CLOCK_BASE_BROKEN,
	.quirks2 = SDHCI_QUIRK2_PRESET_VALUE_BROKEN |
		   SDHCI_QUIRK2_BROKEN_HS200,
	.ops  = &tegra_sdhci_ops,
};

static const struct sdhci_tegra_soc_data soc_data_tegra30 = {
	.pdata = &sdhci_tegra30_pdata,
	.nvquirks = NVQUIRK_ENABLE_SDHCI_SPEC_300 |
		    NVQUIRK_ENABLE_SDR50 |
		    NVQUIRK_ENABLE_SDR104 |
		    NVQUIRK_HAS_PADCALIB,
};

static const struct sdhci_ops tegra114_sdhci_ops = {
	.get_ro     = tegra_sdhci_get_ro,
	.read_w     = tegra_sdhci_readw,
	.write_b    = tegra_sdhci_writeb,
	.write_w    = tegra_sdhci_writew,
	.write_l    = tegra_sdhci_writel,
	.set_clock  = tegra_sdhci_set_clock,
	.set_bus_width = tegra_sdhci_set_bus_width,
	.reset      = tegra_sdhci_reset,
	.platform_execute_tuning = tegra_sdhci_execute_tuning,
	.set_uhs_signaling = tegra_sdhci_set_uhs_signaling,
	.voltage_switch = tegra_sdhci_voltage_switch,
	.get_max_clock = tegra_sdhci_get_max_clock,
};

static const struct sdhci_pltfm_data sdhci_tegra114_pdata = {
	.quirks = SDHCI_QUIRK_BROKEN_TIMEOUT_VAL |
		  SDHCI_QUIRK_DATA_TIMEOUT_USES_SDCLK |
		  SDHCI_QUIRK_SINGLE_POWER_WRITE |
		  SDHCI_QUIRK_NO_HISPD_BIT |
		  SDHCI_QUIRK_BROKEN_ADMA_ZEROLEN_DESC |
		  SDHCI_QUIRK_CAP_CLOCK_BASE_BROKEN,
	.quirks2 = SDHCI_QUIRK2_PRESET_VALUE_BROKEN,
	.ops  = &tegra114_sdhci_ops,
};

static const struct sdhci_tegra_soc_data soc_data_tegra114 = {
	.pdata = &sdhci_tegra114_pdata,
};

static const struct sdhci_pltfm_data sdhci_tegra124_pdata = {
	.quirks = SDHCI_QUIRK_BROKEN_TIMEOUT_VAL |
		  SDHCI_QUIRK_DATA_TIMEOUT_USES_SDCLK |
		  SDHCI_QUIRK_SINGLE_POWER_WRITE |
		  SDHCI_QUIRK_NO_HISPD_BIT |
		  SDHCI_QUIRK_BROKEN_ADMA_ZEROLEN_DESC |
		  SDHCI_QUIRK_CAP_CLOCK_BASE_BROKEN,
	.quirks2 = SDHCI_QUIRK2_PRESET_VALUE_BROKEN |
		   /*
		    * The TRM states that the SD/MMC controller found on
		    * Tegra124 can address 34 bits (the maximum supported by
		    * the Tegra memory controller), but tests show that DMA
		    * to or from above 4 GiB doesn't work. This is possibly
		    * caused by missing programming, though it's not obvious
		    * what sequence is required. Mark 64-bit DMA broken for
		    * now to fix this for existing users (e.g. Nyan boards).
		    */
		   SDHCI_QUIRK2_BROKEN_64_BIT_DMA,
	.ops  = &tegra114_sdhci_ops,
};

static const struct sdhci_tegra_soc_data soc_data_tegra124 = {
	.pdata = &sdhci_tegra124_pdata,
};

static const struct sdhci_pltfm_data sdhci_tegra210_pdata = {
	.quirks = SDHCI_QUIRK_BROKEN_TIMEOUT_VAL |
		  SDHCI_QUIRK_SINGLE_POWER_WRITE |
		  SDHCI_QUIRK_NO_HISPD_BIT |
		  SDHCI_QUIRK_BROKEN_ADMA_ZEROLEN_DESC |
		  SDHCI_QUIRK_CAP_CLOCK_BASE_BROKEN,
	.quirks2 = SDHCI_QUIRK2_PRESET_VALUE_BROKEN,
	.ops  = &tegra_sdhci_ops,
};

static const struct sdhci_tegra_soc_data soc_data_tegra210 = {
	.pdata = &sdhci_tegra210_pdata,
	.nvquirks = NVQUIRK_HW_TAP_CONFIG |
		    NVQUIRK_DIS_CARD_CLK_CONFIG_TAP |
		    NVQUIRK_READ_REG_AFTER_WRITE |
		    NVQUIRK_ENABLE_SDHCI_SPEC_300 |
		    NVQUIRK_ENABLE_SDR50 |
		    NVQUIRK_ENABLE_DDR50 |
		    NVQUIRK_ENABLE_SDR104 |
		    SDHCI_MISC_CTRL_ENABLE_SDR50,
};

static const struct sdhci_pltfm_data sdhci_tegra186_pdata = {
	.quirks = SDHCI_QUIRK_SINGLE_POWER_WRITE |
		  SDHCI_QUIRK_NO_HISPD_BIT |
		SDHCI_QUIRK_DATA_TIMEOUT_USES_SDCLK |
		SDHCI_QUIRK_NO_HISPD_BIT |
		SDHCI_QUIRK_BROKEN_ADMA_ZEROLEN_DESC |
		SDHCI_QUIRK_NO_ENDATTR_IN_NOPDESC,
	.quirks2 = SDHCI_QUIRK2_PRESET_VALUE_BROKEN |
		SDHCI_QUIRK2_USE_64BIT_ADDR |
		SDHCI_QUIRK2_HOST_OFF_CARD_ON,
	.ops  = &tegra_sdhci_ops,
};

static const struct sdhci_tegra_soc_data soc_data_tegra186 = {
	.pdata = &sdhci_tegra186_pdata,
	.nvquirks = NVQUIRK_HW_TAP_CONFIG |
		    NVQUIRK_ENABLE_SDHCI_SPEC_300 |
		    NVQUIRK_ENABLE_SDR50 |
		    NVQUIRK_ENABLE_DDR50 |
		    NVQUIRK_ENABLE_SDR104 |
		    SDHCI_MISC_CTRL_ENABLE_SDR50,
};

static const struct of_device_id sdhci_tegra_dt_match[] = {
	{ .compatible = "nvidia,tegra186-sdhci", .data = &soc_data_tegra186 },
	{ .compatible = "nvidia,tegra210-sdhci", .data = &soc_data_tegra210 },
	{ .compatible = "nvidia,tegra124-sdhci", .data = &soc_data_tegra124 },
	{ .compatible = "nvidia,tegra114-sdhci", .data = &soc_data_tegra114 },
	{ .compatible = "nvidia,tegra30-sdhci", .data = &soc_data_tegra30 },
	{ .compatible = "nvidia,tegra20-sdhci", .data = &soc_data_tegra20 },
	{}
};
MODULE_DEVICE_TABLE(of, sdhci_tegra_dt_match);

static int sdhci_tegra_parse_dt(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct sdhci_host *host = platform_get_drvdata(pdev);
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_tegra *tegra_host = sdhci_pltfm_priv(pltfm_host);
	int val;

	if (!np)
		return -EINVAL;

	of_property_read_u32(np, "max-clk-limit", (u32 *)&tegra_host->max_clk_limit);
	of_property_read_u32(np, "ddr-clk-limit",
		(u32 *)&tegra_host->max_ddr_clk_limit);
	of_property_read_u32(np, "dqs-trim-delay",
		(u32 *)&tegra_host->dqs_trim_delay);
	tegra_host->pwrdet_support = of_property_read_bool(np,
		"pwrdet-support");
	tegra_host->cd_gpio = of_get_named_gpio(np, "cd-gpios", 0);
	tegra_host->cd_wakeup_capable = of_property_read_bool(np,
		"nvidia,cd-wakeup-capable");
#ifdef CONFIG_MMC_CQ_HCI
	tegra_host->enable_hwcq = of_property_read_bool(np, "nvidia,enable-hwcq");
#endif
	host->ocr_mask = MMC_VDD_27_36 | MMC_VDD_165_195;
	if (!of_property_read_u32(np, "mmc-ocr-mask", &val)) {
		if (val == 0)
			host->ocr_mask &= MMC_VDD_165_195;
		else if (val == 1)
			host->ocr_mask &= ~(MMC_VDD_26_27 | MMC_VDD_27_28);
		else if (val == 2)
			host->ocr_mask &= (MMC_VDD_32_33 | MMC_VDD_165_195);
		else if (val == 3)
			host->ocr_mask &= (MMC_VDD_33_34 | MMC_VDD_165_195);
	}
	return 0;
}

static int sdhci_tegra_probe(struct platform_device *pdev)
{
	const struct of_device_id *match;
	const struct sdhci_tegra_soc_data *soc_data;
	struct sdhci_host *host;
	struct sdhci_pltfm_host *pltfm_host;
	struct sdhci_tegra *tegra_host;
	struct clk *clk;
	struct sdhci_tegra_clk_src_data *clk_src_data;
	int rc;

	match = of_match_device(sdhci_tegra_dt_match, &pdev->dev);
	if (!match)
		return -EINVAL;
	soc_data = match->data;

	host = sdhci_pltfm_init(pdev, soc_data->pdata, sizeof(*tegra_host));
	if (IS_ERR(host))
		return PTR_ERR(host);
	pltfm_host = sdhci_priv(host);

	tegra_host = sdhci_pltfm_priv(pltfm_host);
	tegra_host->ddr_signaling = false;
	tegra_host->pad_calib_required = false;

	/* FIXME: This is for until dma-mask binding is supported in DT.
	 *        Set coherent_dma_mask for each Tegra SKUs.
	 *        If dma_mask is NULL, set it to coherent_dma_mask. */
	pdev->dev.coherent_dma_mask = DMA_BIT_MASK(64);

	if (!pdev->dev.dma_mask)
		pdev->dev.dma_mask = &pdev->dev.coherent_dma_mask;

	tegra_host->soc_data = soc_data;

	rc = mmc_of_parse(host->mmc);
	if (rc)
		goto err_parse_dt;
	sdhci_tegra_parse_dt(pdev);

	clk_src_data = devm_kzalloc(&pdev->dev, sizeof(clk_src_data),
		GFP_KERNEL);
	if (IS_ERR_OR_NULL(clk_src_data)) {
		dev_err(mmc_dev(host->mmc),
			"Insufficient memory for clk source data\n");
		return -ENOMEM;
	}
	tegra_host->clk_src_data = clk_src_data;
	rc = sdhci_tegra_get_parent_pll_from_dt(host, pdev);
	if (rc)
		dev_err(mmc_dev(host->mmc),
			"Failed to find parent clocks\n");

	tegra_host->prods = devm_tegra_prod_get(&pdev->dev);
	if (IS_ERR_OR_NULL(tegra_host->prods)) {
		dev_err(mmc_dev(host->mmc), "Prod-setting not available\n");
		tegra_host->prods = NULL;
	}

	if (tegra_host->pwrdet_support) {
		tegra_host->sdmmc_padctrl =
			devm_padctrl_get(&pdev->dev, "sdmmc");
		if (IS_ERR(tegra_host->sdmmc_padctrl)) {
			dev_err(mmc_dev(host->mmc),
				"Pad control not found %ld\n",
				PTR_ERR(tegra_host->sdmmc_padctrl));
			tegra_host->sdmmc_padctrl = NULL;
		}
	}

	if (tegra_host->soc_data->nvquirks & NVQUIRK_ENABLE_DDR50)
		host->mmc->caps |= MMC_CAP_1_8V_DDR;

	tegra_host->power_gpio = devm_gpiod_get_optional(&pdev->dev, "power",
							 GPIOD_OUT_HIGH);
	if (IS_ERR(tegra_host->power_gpio)) {
		rc = PTR_ERR(tegra_host->power_gpio);
		goto err_power_req;
	}

	clk = devm_clk_get(&pdev->dev, "sdmmc");
	if (IS_ERR(clk)) {
		dev_err(mmc_dev(host->mmc), "clk err\n");
		rc = PTR_ERR(clk);
		if (!tegra_platform_is_vdk())
			goto err_clk_get;
	}
	clk_prepare_enable(clk);

	tegra_host->rst = devm_reset_control_get(&pdev->dev, "sdmmc");
	if (IS_ERR(tegra_host->rst))
		dev_err(mmc_dev(host->mmc), "reset err\n");
	else
		reset_control_reset(tegra_host->rst);

	pltfm_host->clk = clk;

	if (gpio_is_valid(tegra_host->cd_gpio) &&
			tegra_host->cd_wakeup_capable) {
		tegra_host->cd_irq = gpio_to_irq(tegra_host->cd_gpio);
		if (tegra_host->cd_irq <= 0) {
			dev_err(mmc_dev(host->mmc),
				"failed to get gpio irq %d\n",
				tegra_host->cd_irq);
			tegra_host->cd_irq = 0;
		} else {
			device_init_wakeup(&pdev->dev, 1);
			dev_info(mmc_dev(host->mmc),
				"wakeup init done, cdirq %d\n",
				tegra_host->cd_irq);
		}
	}

	if (!en_boot_part_access)
		host->mmc->caps2 |= MMC_CAP2_BOOTPART_NOACC;

	if (tegra_platform_is_vdk())
		host->mmc->caps |= MMC_CAP2_NO_EXTENDED_GP;
#ifdef CONFIG_MMC_CQ_HCI
	if (tegra_host->enable_hwcq) {
		host->mmc->caps2 |= MMC_CAP2_HW_CQ;
		host->cq_host = cmdq_pltfm_init(pdev);
		if (IS_ERR(host->cq_host))
			pr_err("CMDQ: Error in cmdq_platfm_init function\n");
		else
			pr_info("CMDQ: cmdq_platfm_init successful\n");
	}
#endif

	rc = sdhci_add_host(host);
	if (rc)
		goto err_add_host;

	/* Initialize debugfs */
	sdhci_tegra_debugfs_init(host);

	return 0;

err_add_host:
	clk_disable_unprepare(pltfm_host->clk);
err_clk_get:
err_power_req:
err_parse_dt:
	sdhci_pltfm_free(pdev);
	return rc;
}

static int tegra_sdhci_suspend(struct sdhci_host *host)
{
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_tegra *tegra_host = sdhci_pltfm_priv(pltfm_host);
	struct platform_device *pdev = to_platform_device(mmc_dev(host->mmc));

	/* Enable wake irq at end of suspend */
	if (device_may_wakeup(&pdev->dev)) {
		if (enable_irq_wake(tegra_host->cd_irq)) {
			dev_err(mmc_dev(host->mmc),
				"Failed to enable wake irq %u\n",
				tegra_host->cd_irq);
			tegra_host->wake_enable_failed = true;
		}
	}

	return 0;
}

static int tegra_sdhci_resume(struct sdhci_host *host)
{
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_tegra *tegra_host = sdhci_pltfm_priv(pltfm_host);
	struct platform_device *pdev = to_platform_device(mmc_dev(host->mmc));
	int ret = 0;

	if (device_may_wakeup(&pdev->dev)) {
		if (!tegra_host->wake_enable_failed) {
			ret = disable_irq_wake(tegra_host->cd_irq);
			if (ret)
				dev_err(mmc_dev(host->mmc),
					"Failed to disable wakeirq %u,err %d\n",
					tegra_host->cd_irq, ret);
		}
	}

	/* Set min identificaion clock of 400 KHz */
	tegra_sdhci_set_clock(host, 400000);

	return ret;
}

static void tegra_sdhci_post_resume(struct sdhci_host *host)
{
	bool dll_calib_req = false;

	dll_calib_req = (host->mmc->card && mmc_card_mmc(host->mmc->card) &&
		(host->mmc->ios.timing == MMC_TIMING_MMC_HS400));
	if (dll_calib_req)
		tegra_sdhci_post_init(host);

}

static void sdhci_tegra_debugfs_init(struct sdhci_host *host)
{
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_tegra *tegra_host = sdhci_pltfm_priv(pltfm_host);
	struct sdhci_tegra_clk_src_data *clk_src_data;
	struct dentry *sdhcidir, *clkdir, *retval;

	clk_src_data = tegra_host->clk_src_data;
	sdhcidir = debugfs_create_dir(dev_name(mmc_dev(host->mmc)), NULL);
	if (!sdhcidir) {
		dev_err(mmc_dev(host->mmc), "Failed to create debugfs\n");
		return;
	}

	/* Create clock debugfs dir under sdhci debugfs dir */
	clkdir = debugfs_create_dir("clock_data", sdhcidir);
	if (!clkdir)
		goto err;

	retval = debugfs_create_ulong("curr_clk_rate", S_IRUGO, clkdir,
		&tegra_host->curr_clk_rate);
	if (!retval)
		goto err;

	retval = debugfs_create_ulong("parent_clk_rate", S_IRUGO, clkdir,
		&clk_src_data->parent_clk_rate[
			clk_src_data->curr_parent_clk_idx]);
	if (!retval)
		goto err;

	return;
err:
	debugfs_remove_recursive(sdhcidir);
	sdhcidir = NULL;
	return;
}

static struct platform_driver sdhci_tegra_driver = {
	.driver		= {
		.name	= "sdhci-tegra",
		.of_match_table = sdhci_tegra_dt_match,
		.pm	= &sdhci_pltfm_pmops,
	},
	.probe		= sdhci_tegra_probe,
	.remove		= sdhci_pltfm_unregister,
};

module_platform_driver(sdhci_tegra_driver);

module_param(en_boot_part_access, uint, 0444);

MODULE_DESCRIPTION("SDHCI driver for Tegra");
MODULE_AUTHOR("Google, Inc.");
MODULE_LICENSE("GPL v2");
