// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2023 Intel Corporation. */

/*
 *  X86 Resource Control Driver to show closid/rmid
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/resctrl.h>
#include <linux/cacheinfo.h>
#include <linux/seq_file.h>

#include "rdt.h"

#undef pr_fmt
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

static int closid_show(struct seq_file *sf, u64 resctrl_ids)
{
	seq_printf(sf, "%lld\n", resctrl_ids >> 32);

	return 0;
}

static int rmid_show(struct seq_file *sf, u64 resctrl_ids)
{
	seq_printf(sf, "%lld\n", resctrl_ids & 0xffff);

	return 0;
}

static struct resctrl_ctrlfileinfo files[] = {
	{
		.name	= "closid",
		.show	= closid_show,
		.flags	= RESCTRL_CTRLMON_FILE | RESCTRL_MON_FILE,
	},
	{
		.name	= "rmid",
		.show	= rmid_show,
		.flags	= RESCTRL_CTRLMON_FILE | RESCTRL_MON_FILE,
	},
	{ }
};

static struct resctrl_resource show = {
	.ctrlfiles	= files,
};

static int __init show_init(void)
{
	return resctrl_register_resource(&show);
}

static void __exit show_cleanup(void)
{
	resctrl_unregister_resource(&show);
}

module_init(show_init);
module_exit(show_cleanup);

MODULE_AUTHOR("Tony Luck <tony.luck@intel.com>");
MODULE_IMPORT_NS(RESCTRL);
MODULE_LICENSE("GPL");
