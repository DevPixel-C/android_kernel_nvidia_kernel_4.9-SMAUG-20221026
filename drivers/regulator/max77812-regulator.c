/*
 * MAXIM max77812 Regulator driver
 *
 * Copyright (C) 2017 NVIDIA CORPORATION. All rights reserved.
 *
 * Author: Venkat Reddy Talla <vreddytalla@nvidia.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed "as is" WITHOUT ANY WARRANTY of any kind,
 * whether express or implied; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */

#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>

#define max77812_rails(_name)	"max77812-"#_name

#define MAX77812_REG_RSET		0x00
#define MAX77812_REG_INT_SRC		0x01
#define MAX77812_REG_INT_SRC_M		0x02
#define MAX77812_REG_TOPSYS_INT		0x03
#define MAX77812_REG_TOPSYS_INT_M	0x04
#define MAX77812_REG_TOPSYS_STAT	0x05
#define MAX77812_REG_EN_CTRL		0x06
#define MAX77812_REG_STUP_DLY2		0x07
#define MAX77812_REG_STUP_DLY3		0x08
#define MAX77812_REG_STUP_DLY4		0x09
#define MAX77812_REG_SHDN_DLY1		0x0A
#define MAX77812_REG_SHDN_DLY2		0x0B
#define MAX77812_REG_SHDN_DLY3		0x0C
#define MAX77812_REG_SHDN_DLY4		0x0D
#define MAX77812_REG_WDTRSTB_DEB	0x0E
#define MAX77812_REG_GPI_FUNC		0x0F
#define MAX77812_REG_GPI_DEB1		0x10
#define MAX77812_REG_GPI_DEB2		0x11
#define MAX77812_REG_GPI_PD_CTRL	0x12
#define MAX77812_REG_PROT_CFG		0x13
#define MAX77812_REG_I2C_CFG		0x15
#define MAX77812_REG_BUCK_INT		0x20
#define MAX77812_REG_BUCK_INT_M		0x21
#define MAX77812_REG_BUCK_STAT		0x22
#define MAX77812_REG_M1_VOUT		0x23
#define MAX77812_REG_M2_VOUT		0x24
#define MAX77812_REG_M3_VOUT		0x25
#define MAX77812_REG_M4_VOUT		0x26
#define MAX77812_REG_M1_VOUT_D		0x27
#define MAX77812_REG_M2_VOUT_D		0x28
#define MAX77812_REG_M3_VOUT_D		0x29
#define MAX77812_REG_M4_VOUT_D		0x2A
#define MAX77812_REG_M1_VOUT_S		0x2B
#define MAX77812_REG_M2_VOUT_S		0x2C
#define MAX77812_REG_M3_VOUT_S		0x2D
#define MAX77812_REG_M4_VOUT_S		0x2E
#define MAX77812_REG_M1_CGF		0x2F
#define MAX77812_REG_M2_CGF		0x30
#define MAX77812_REG_M3_CGF		0x31
#define MAX77812_REG_M4_CGF		0x32
#define MAX77812_REG_GLB_CFG1		0x33
#define MAX77812_REG_GLB_CFG2		0x34
#define MAX77812_REG_GLB_CFG3		0x35
#define MAX77812_REG_GLB_CFG4		0x36
#define MAX77812_REG_GLB_CFG5		0x37
#define MAX77812_REG_GLB_CFG6		0x38
#define MAX77812_REG_GLB_CFG7		0x39
#define MAX77812_REG_GLB_CFG8		0x3A
#define MAX77812_REG_PROT_ACCESS	0xFD
#define MAX77812_REG_MAX		0xFE

#define MAX77812_REG_EN_CTRL_MASK(n)		BIT(n)
#define MAX77812_START_SLEW_RATE_MASK		0x07
#define MAX77812_SHDN_SLEW_RATE_MASK		0x70
#define MAX77812_RAMPDOWN_SLEW_RATE_MASK	0x07
#define MAX77812_RAMPUP_SLEW_RATE_MASK		0x70
#define MAX77812_SLEW_RATE_SHIFT		4

#define MAX77812_OP_ACTIVE_DISCHARGE_MASK		BIT(7)
#define MAX77812_PEAK_CURRENT_LMT_MASK			0x70
#define MAX77812_SWITCH_FREQ_MASK			0x0C
#define MAX77812_FORCED_PWM_MASK			BIT(1)
#define MAX77812_SLEW_RATE_CNTRL_MASK			BIT(0)
#define MAX77812_START_SHD_DELAY_MASK			0x1F

#define MAX77812_VOUT_MASK		0xFF
#define MAX77812_VOUT_N_VOLTAGE		0xFF
#define MAX77812_VOUT_VMIN		250000
#define MAX77812_VOUT_VMAX		1525000
#define MAX77812_VOUT_STEP		5000

#define MAX77812_REGULATOR_ID_M1	0
#define MAX77812_REGULATOR_ID_M2	1
#define MAX77812_REGULATOR_ID_M3	2
#define MAX77812_REGULATOR_ID_M4	3
#define MAX77812_MAX_REGULATORS		4

static unsigned int slew_rate_table[] = {
	1250, 2500, 5000, 10000, 20000, 40000, 60000,
};

static unsigned int peak_current_limit[] = {
	3000000, 3600000, 4200000, 4800000, 5400000, 6000000,
	6000000, 7200000,
};

struct max77812_reg_pdata {
	struct regulator_init_data *ridata;
	int peak_current_limit;
	int switching_freq;
	bool disable_active_discharge;
	bool delay_time_step_select;
	bool enable_forced_pwm;
	bool disable_slew_rate_cntrl;
};

struct max77812_regulator {
	struct device *dev;
	struct regmap *rmap;
	struct regulator_desc *rdesc[MAX77812_MAX_REGULATORS];
	struct max77812_reg_pdata reg_pdata[MAX77812_MAX_REGULATORS];
	struct regulator_dev *rdev[MAX77812_MAX_REGULATORS];
	u32 ramp_up_slew_rate;
	u32 ramp_down_slew_rate;
	u32 shutdown_slew_rate;
	u32 softstart_slew_rate;
	bool skip_protect_reg_access;
};

static int max77812_regulator_enable(struct regulator_dev *rdev)
{
	struct max77812_regulator *max77812 = rdev_get_drvdata(rdev);
	int ret;

	ret = regmap_update_bits(max77812->rmap, rdev->desc->enable_reg,
				 rdev->desc->enable_mask,
				 rdev->desc->enable_mask);
	if (ret < 0) {
		dev_err(max77812->dev, "Regulator enable failed: %d\n", ret);
		return ret;
	}

	return ret;
}

static int max77812_regulator_disable(struct regulator_dev *rdev)
{
	struct max77812_regulator *max77812 = rdev_get_drvdata(rdev);
	int ret;

	ret = regmap_update_bits(max77812->rmap, rdev->desc->enable_reg,
				 rdev->desc->enable_mask, 0);
	if (ret < 0) {
		dev_err(max77812->dev, "Regulator disable failed: %d\n", ret);
		return ret;
	}

	return ret;
}

static u8 max77802_slew_rate_to_reg(const unsigned int sr_limits[],
				    u8 cnt, unsigned int slew_rate)
{
	int i;

	for (i = 0; i < cnt; i++) {
		if (slew_rate <= sr_limits[i])
			return i;
	}

	return cnt - 1;
}

static int max77812_reg_init(struct max77812_regulator *max77812)
{
	u8 slew_rate = 0;
	u8 rampdelay = 0;
	u8 mask = 0;
	unsigned int val;
	int ret;

	if (max77812->softstart_slew_rate) {
		slew_rate = max77802_slew_rate_to_reg(slew_rate_table,
				ARRAY_SIZE(slew_rate_table),
				max77812->softstart_slew_rate);
		mask = MAX77812_START_SLEW_RATE_MASK;
	}

	if (max77812->shutdown_slew_rate) {
		rampdelay = max77802_slew_rate_to_reg(slew_rate_table,
				ARRAY_SIZE(slew_rate_table),
				max77812->shutdown_slew_rate);
		slew_rate |= (rampdelay << MAX77812_SLEW_RATE_SHIFT);
		mask |= MAX77812_SHDN_SLEW_RATE_MASK;
	}

	if (slew_rate && mask) {
		ret = regmap_update_bits(max77812->rmap,
					 MAX77812_REG_GLB_CFG1,
					 mask, slew_rate);
		if (ret < 0) {
			dev_err(max77812->dev,
				"slew rate cfg1 update failed %d\n", ret);
			return ret;
		}
	}

	slew_rate = 0;
	mask = 0;

	if (max77812->ramp_up_slew_rate) {
		slew_rate = max77802_slew_rate_to_reg(slew_rate_table,
				ARRAY_SIZE(slew_rate_table),
				max77812->ramp_up_slew_rate);
		mask = MAX77812_RAMPUP_SLEW_RATE_MASK;
	}

	if (max77812->ramp_down_slew_rate) {
		rampdelay = max77802_slew_rate_to_reg(slew_rate_table,
				ARRAY_SIZE(slew_rate_table),
				max77812->ramp_down_slew_rate);
		slew_rate |= (rampdelay << MAX77812_SLEW_RATE_SHIFT);
		mask |= MAX77812_RAMPDOWN_SLEW_RATE_MASK;
	}

	if (slew_rate && mask) {
		ret = regmap_update_bits(max77812->rmap,
					 MAX77812_REG_GLB_CFG2,
					 mask, slew_rate);
		if (ret < 0) {
			dev_err(max77812->dev,
				"slew rate cfg2 update failed %d\n", ret);
			return ret;
		}
	}

	if (!max77812->skip_protect_reg_access) {
		ret = regmap_write(max77812->rmap,
				   MAX77812_REG_PROT_ACCESS, 0x5A);
		if (ret < 0)
			goto error;

		ret = regmap_read(max77812->rmap,
				  MAX77812_REG_PROT_ACCESS, &val);
		if (ret < 0)
			goto error;

		if (val != 0x5A) {
			dev_err(max77812->dev, "prot register unlock failed\n");
			return -EINVAL;
		}

		ret = regmap_write(max77812->rmap, MAX77812_REG_GLB_CFG5, 0x3E);
		if (ret < 0)
			goto error;

		ret = regmap_write(max77812->rmap, MAX77812_REG_GLB_CFG6, 0x90);
		if (ret < 0)
			goto error;

		ret = regmap_write(max77812->rmap, MAX77812_REG_GLB_CFG8, 0x3A);
		if (ret < 0)
			goto error;

		ret = regmap_write(max77812->rmap,
				   MAX77812_REG_PROT_ACCESS, 0x0);
		if (ret < 0)
			goto error;

		ret = regmap_read(max77812->rmap,
				  MAX77812_REG_PROT_ACCESS, &val);
		if (ret < 0)
			goto error;

		if (val) {
			dev_err(max77812->dev, "protect registers lock failed\n");
			return -EINVAL;
		}
	}

	return 0;

error:
	dev_err(max77812->dev, "protect register access failed %d\n", ret);
	return ret;
}

static int max77812_config_init(struct max77812_regulator *max77812, int id)
{
	struct max77812_reg_pdata *rpdata = &max77812->reg_pdata[id];
	u8 rail_config = 0;
	u8 mask = 0;
	u8 curnt_lim;
	u8 switch_freq;
	u8 reg_addr;
	int ret;

	if (rpdata->disable_active_discharge) {
		rail_config &= ~MAX77812_OP_ACTIVE_DISCHARGE_MASK;
		mask = MAX77812_OP_ACTIVE_DISCHARGE_MASK;
	}

	if (rpdata->peak_current_limit > 0) {
		curnt_lim = max77802_slew_rate_to_reg(peak_current_limit,
				ARRAY_SIZE(peak_current_limit),
				rpdata->peak_current_limit);
		rail_config |= (curnt_lim << 4);
		mask |= MAX77812_PEAK_CURRENT_LMT_MASK;
	}

	if (rpdata->switching_freq > 0) {
		if (rpdata->switching_freq == 2)
			switch_freq = 0;
		else if (rpdata->switching_freq == 3)
			switch_freq = 1;
		else if (rpdata->switching_freq == 4)
			switch_freq = 2;
		else
			switch_freq = 3;
		rail_config |= (switch_freq << 2);
		mask |= MAX77812_SWITCH_FREQ_MASK;
	}

	if (rpdata->enable_forced_pwm) {
		rail_config |= MAX77812_FORCED_PWM_MASK;
		mask |= MAX77812_FORCED_PWM_MASK;
	}

	if (rpdata->disable_slew_rate_cntrl) {
		rail_config &= ~MAX77812_SLEW_RATE_CNTRL_MASK;
		mask |= MAX77812_SLEW_RATE_CNTRL_MASK;
	}

	switch (id) {
	case MAX77812_REGULATOR_ID_M1:
		reg_addr = MAX77812_REG_M1_CGF;
		break;
	case MAX77812_REGULATOR_ID_M2:
		reg_addr = MAX77812_REG_M2_CGF;
		break;
	case MAX77812_REGULATOR_ID_M3:
		reg_addr = MAX77812_REG_M3_CGF;
		break;
	case MAX77812_REGULATOR_ID_M4:
		reg_addr = MAX77812_REG_M4_CGF;
		break;
	}

	if (rail_config && mask) {
		ret = regmap_update_bits(max77812->rmap,
					 reg_addr, mask, rail_config);
		if (ret < 0) {
			dev_err(max77812->dev,
				"reg config update failed %d\n", ret);
			return ret;
		}
	}

	return 0;
}

static int max77812_of_parse_cb(struct device_node *np,
				const struct regulator_desc *desc,
				struct regulator_config *config)
{
	struct max77812_regulator *max77812 = config->driver_data;
	struct max77812_reg_pdata *rpdata = &max77812->reg_pdata[desc->id];
	u32 pval;
	int ret;

	rpdata->disable_active_discharge = of_property_read_bool(np,
				"maxim,disable-active-discharge");

	ret = of_property_read_u32(np, "maxim,peak-current-limit-ua", &pval);
	rpdata->peak_current_limit = (!ret) ? pval : -1;

	ret = of_property_read_u32(np, "maxim,switching-frequency", &pval);
	rpdata->switching_freq = (!ret) ? pval : -1;

	rpdata->enable_forced_pwm = of_property_read_bool(np,
				"maxim,enable-forced-pwm-mode");

	rpdata->disable_slew_rate_cntrl = of_property_read_bool(np,
				"maxim,disable-slew-rate-control");

	return max77812_config_init(max77812, desc->id);
}

static struct regulator_ops max77812_regulator_ops = {
	.enable = max77812_regulator_enable,
	.disable = max77812_regulator_disable,
	.list_voltage = regulator_list_voltage_linear,
	.map_voltage = regulator_map_voltage_linear,
	.get_voltage_sel = regulator_get_voltage_sel_regmap,
	.set_voltage_sel = regulator_set_voltage_sel_regmap,
};

#define MAX77812_REGULATOR_DESC(_id, _name, _en_bit)		\
	[MAX77812_REGULATOR_ID_##_id] = {		\
		.name = max77812_rails(_name),		\
		.of_match = of_match_ptr(#_name),		\
		.regulators_node = of_match_ptr("regulators"),	\
		.of_parse_cb = max77812_of_parse_cb,		\
		.supply_name = "vin",			\
		.id = MAX77812_REGULATOR_ID_##_id,	\
		.ops = &max77812_regulator_ops,		\
		.n_voltages = MAX77812_VOUT_N_VOLTAGE,	\
		.min_uV = MAX77812_VOUT_VMIN,		\
		.uV_step = MAX77812_VOUT_STEP,		\
		.enable_time = 500,			\
		.vsel_mask = MAX77812_VOUT_MASK,	\
		.vsel_reg = MAX77812_REG_##_id##_VOUT,	\
		.enable_reg	= MAX77812_REG_EN_CTRL,		\
		.enable_mask = BIT(_en_bit),			\
		.type = REGULATOR_VOLTAGE,		\
		.owner = THIS_MODULE,			\
	}

static struct regulator_desc max77812_regs_desc[MAX77812_MAX_REGULATORS] = {
	MAX77812_REGULATOR_DESC(M1, m1vout, 0),
	MAX77812_REGULATOR_DESC(M2, m2vout, 2),
	MAX77812_REGULATOR_DESC(M3, m3vout, 4),
	MAX77812_REGULATOR_DESC(M4, m4vout, 6),
};

static int max77812_reg_parse_dt(struct device *dev,
				 struct max77812_regulator *max77812_regs)
{
	struct device_node *np = dev->of_node;
	u32 pval;
	int ret;

	ret = of_property_read_u32(np, "maxim,ramp-up-slew-rate", &pval);
	if (!ret)
		max77812_regs->ramp_up_slew_rate = pval;

	ret = of_property_read_u32(np, "maxim,ramp-down-slew-rate", &pval);
	if (!ret)
		max77812_regs->ramp_down_slew_rate = pval;

	ret = of_property_read_u32(np, "maxim,shutdown-slew-rate", &pval);
	if (!ret)
		max77812_regs->shutdown_slew_rate = pval;

	ret = of_property_read_u32(np, "maxim,soft-start-slew-rate", &pval);
	if (!ret)
		max77812_regs->softstart_slew_rate = pval;

	max77812_regs->skip_protect_reg_access = of_property_read_bool(np,
				"maxim,skip-protect-reg-access");

	return 0;
}

static const struct regmap_config max77812_regmap_config = {
	.reg_bits		= 8,
	.val_bits		= 8,
	.max_register	= MAX77812_REG_MAX - 1,
	.cache_type		= REGCACHE_NONE,
};

static int max77812_probe(struct i2c_client *client,
			  const struct i2c_device_id *client_id)
{
	struct device *dev = &client->dev;
	struct max77812_reg_pdata *rpdata;
	struct regulator_config config = { };
	struct regulator_desc *rdesc;
	struct max77812_regulator *max77812;
	int id;
	int ret;

	max77812 = devm_kzalloc(dev, sizeof(*max77812), GFP_KERNEL);
	if (!max77812)
		return -ENOMEM;

	ret = max77812_reg_parse_dt(dev, max77812);
	if (ret < 0) {
		dev_err(dev, "Reading data from DT failed: %d\n", ret);
		return ret;
	}

	max77812->rmap = devm_regmap_init_i2c(client, &max77812_regmap_config);
	if (IS_ERR(max77812->rmap)) {
		ret = PTR_ERR(max77812->rmap);
		dev_err(dev, "regmap init failed: %d\n", ret);
		return ret;
	}

	i2c_set_clientdata(client, max77812);
	max77812->dev = dev;

	ret = max77812_reg_init(max77812);
	if (ret < 0) {
		dev_err(dev, "max77812 Init failed: %d\n", ret);
		return ret;
	}

	for (id = 0; id < MAX77812_MAX_REGULATORS; ++id) {
		rdesc = &max77812_regs_desc[id];
		max77812->rdesc[id] = rdesc;
		rpdata = &max77812->reg_pdata[id];

		config.regmap = max77812->rmap;
		config.dev = dev;
		config.init_data = rpdata->ridata;
		config.driver_data = max77812;

		max77812->rdev[id] = devm_regulator_register(dev,
					rdesc, &config);
		if (IS_ERR(max77812->rdev[id])) {
			ret = PTR_ERR(max77812->rdev[id]);
			dev_err(dev, "regulator %s register failed: %d\n",
				rdesc->name, ret);
			return ret;
		}
	}

	return 0;
}

static const struct of_device_id max77812_of_match[] = {
	{ .compatible = "maxim,max77812-regulator", },
	{},
};
MODULE_DEVICE_TABLE(of, max77812_of_match);

static const struct i2c_device_id max77812_id[] = {
	{.name = "max77812",},
	{},
};

static struct i2c_driver max77812_i2c_driver = {
	.driver = {
		.name = "max77812",
		.owner = THIS_MODULE,
		.of_match_table = max77812_of_match,
	},
	.probe = max77812_probe,
	.id_table = max77812_id,
};
module_i2c_driver(max77812_i2c_driver);

MODULE_AUTHOR("Venkat Reddy Talla <vreddytalla@nvidia.com>");
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("max77812 regulator driver");
