// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2023 Intel Corporation. */

/*
 *  X86 Resource Control Driver For L3 cache allocation
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/resctrl.h>

#include <asm/cpu_device_id.h>

#undef pr_fmt
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

static const struct x86_cpu_id cat_feature[] = {
	X86_MATCH_FEATURE(X86_FEATURE_CAT_L3, 0),
	{ }
};
MODULE_DEVICE_TABLE(x86cpu, cat_feature);

static struct resctrl_fileinfo cat_files[] = {
	{
		.name	= "cbm_mask",
	},
	{
		.name	= "min_cbm_bits",
	},
	{
		.name	= "num_closids",
	},
	{ }
};

static struct resctrl_resource cat = {
	.name		= "L3",
	.infodir	= "L3",
	.infofiles	= cat_files,
};

static int __init cat_init(void)
{
	if (!boot_cpu_has(X86_FEATURE_RDT_A)) {
		pr_debug("No RDT allocation support\n");
		return -ENODEV;
	}
	if (!x86_match_cpu(cat_feature)) {
		pr_debug("No support for CAT L3\n");
		return -ENODEV;
	}

	return resctrl_register_resource(&cat);
}

static void __exit cat_cleanup(void)
{
	resctrl_unregister_resource(&cat);
}

module_init(cat_init);
module_exit(cat_cleanup);

MODULE_AUTHOR("Tony Luck <tony.luck@intel.com>");
MODULE_IMPORT_NS(RESCTRL);
MODULE_LICENSE("GPL");
