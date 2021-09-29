/*
 * Copyright (C) 2014-2017 NVIDIA CORPORATION.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/of.h>

#include <soc/tegra/common.h>
#include <linux/memblock.h>

/* Before T18x architecture */
static const struct of_device_id tegra210_le_machine_match[] = {
	{ .compatible = "nvidia,tegra20", },
	{ .compatible = "nvidia,tegra30", },
	{ .compatible = "nvidia,tegra114", },
	{ .compatible = "nvidia,tegra124", },
	{ .compatible = "nvidia,tegra132", },
	{ .compatible = "nvidia,tegra210", },
	{ }
};

/* T186 and later architecture */
static const struct of_device_id tegra186_ge_machine_match[] = {
	{ .compatible = "nvidia,tegra186", },
	{ .compatible = "nvidia,tegra194", },
	{ }
};

phys_addr_t tegra_bootloader_fb_start;
phys_addr_t tegra_bootloader_fb_size;
phys_addr_t tegra_bootloader_fb2_start;
phys_addr_t tegra_bootloader_fb2_size;
phys_addr_t tegra_bootloader_fb3_start;
phys_addr_t tegra_bootloader_fb3_size;
phys_addr_t tegra_bootloader_lut_start;
phys_addr_t tegra_bootloader_lut_size;
phys_addr_t tegra_fb_start;
phys_addr_t tegra_fb_size;
phys_addr_t tegra_fb2_start;
phys_addr_t tegra_fb2_size;
phys_addr_t tegra_fb3_start;
phys_addr_t tegra_fb3_size;
phys_addr_t tegra_lut_start;
phys_addr_t tegra_lut_size;

bool soc_is_tegra210_n_before(void)
{
	struct device_node *root;

	root = of_find_node_by_path("/");
	if (!root)
		return false;

	return of_match_node(tegra210_le_machine_match, root) != NULL;
}

bool soc_is_tegra186_n_later(void)
{
	struct device_node *root;

	root = of_find_node_by_path("/");
	if (!root)
		return false;

	return of_match_node(tegra186_ge_machine_match, root) != NULL;
}

static int __init tegra_bootloader_fb_arg(char *options)
{
	char *p = options;

	tegra_bootloader_fb_size = memparse(p, &p);
	if (*p == '@')
		tegra_bootloader_fb_start = memparse(p+1, &p);

	pr_info("Found tegra_fbmem: %08llx@%08llx\n",
		(u64)tegra_bootloader_fb_size, (u64)tegra_bootloader_fb_start);

	if (tegra_bootloader_fb_size) {
		tegra_bootloader_fb_size = PAGE_ALIGN(tegra_bootloader_fb_size);

		if (memblock_reserve(tegra_bootloader_fb_start,
				tegra_bootloader_fb_size)) {
			pr_err("Failed to reserve bootloader fb %08llx@%08llx\n",
				(u64)tegra_bootloader_fb_size,
				(u64)tegra_bootloader_fb_start);
			tegra_bootloader_fb_start = 0;
			tegra_bootloader_fb_size = 0;
		}
	}

	return 0;
}
early_param("tegra_fbmem", tegra_bootloader_fb_arg);

static int __init tegra_bootloader_fb2_arg(char *options)
{
	char *p = options;

	tegra_bootloader_fb2_size = memparse(p, &p);
	if (*p == '@')
		tegra_bootloader_fb2_start = memparse(p+1, &p);

	pr_info("Found tegra_fbmem2: %08llx@%08llx\n",
		(u64)tegra_bootloader_fb2_size,
		(u64)tegra_bootloader_fb2_start);

	if (tegra_bootloader_fb2_size) {
		tegra_bootloader_fb2_size =
				PAGE_ALIGN(tegra_bootloader_fb2_size);
		if (memblock_reserve(tegra_bootloader_fb2_start,
				tegra_bootloader_fb2_size)) {
			pr_err("Failed to reserve bootloader fb2 %08llx@%08llx\n",
				(u64)tegra_bootloader_fb2_size,
				(u64)tegra_bootloader_fb2_start);
			tegra_bootloader_fb2_start = 0;
			tegra_bootloader_fb2_size = 0;
		}
	}

	return 0;
}
early_param("tegra_fbmem2", tegra_bootloader_fb2_arg);

static int __init tegra_bootloader_fb3_arg(char *options)
{
	char *p = options;

	tegra_bootloader_fb3_size = memparse(p, &p);
	if (*p == '@')
		tegra_bootloader_fb3_start = memparse(p+1, &p);

	pr_info("Found tegra_fbmem3: %08llx@%08llx\n",
		(u64)tegra_bootloader_fb3_size,
		(u64)tegra_bootloader_fb3_start);

	if (tegra_bootloader_fb3_size) {
		tegra_bootloader_fb3_size =
				PAGE_ALIGN(tegra_bootloader_fb3_size);
		if (memblock_reserve(tegra_bootloader_fb3_start,
				tegra_bootloader_fb3_size)) {
			pr_err("Failed to reserve bootloader fb3 %08llx@%08llx\n",
				(u64)tegra_bootloader_fb3_size,
				(u64)tegra_bootloader_fb3_start);
			tegra_bootloader_fb3_start = 0;
			tegra_bootloader_fb3_size = 0;
		}
	}

	return 0;
}
early_param("tegra_fbmem3", tegra_bootloader_fb3_arg);

static int __init tegra_bootloader_lut_arg(char *options)
{
	char *p = options;

	tegra_bootloader_lut_size = memparse(p, &p);
	if (*p == '@')
		tegra_bootloader_lut_start = memparse(p+1, &p);

	pr_info("Found lut_mem: %08llx@%08llx\n",
		(u64)tegra_bootloader_lut_size,
		(u64)tegra_bootloader_lut_start);

	if (tegra_bootloader_lut_size) {
		tegra_bootloader_lut_size =
				PAGE_ALIGN(tegra_bootloader_lut_size);
		if (memblock_reserve(tegra_bootloader_lut_start,
				tegra_bootloader_lut_size)) {
			pr_err("Failed to reserve bootloader lut_mem %08llx@%08llx\n",
				(u64)tegra_bootloader_lut_size,
				(u64)tegra_bootloader_lut_start);
			tegra_bootloader_lut_start = 0;
			tegra_bootloader_lut_size = 0;
		}
	}

	return 0;
}
early_param("lut_mem", tegra_bootloader_lut_arg);
