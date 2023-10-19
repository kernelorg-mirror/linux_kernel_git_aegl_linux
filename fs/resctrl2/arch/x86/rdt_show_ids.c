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

#include "../../internal.h"

#include "rdt.h"

#undef pr_fmt
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

static int closid_show(struct seq_file *sf, resctrl_ids_t resctrl_ids)
{
	seq_printf(sf, "%lld\n", FIELD_GET(CLOSID_FIELD, resctrl_ids));

	return 0;
}

static int rmid_show(struct seq_file *sf, resctrl_ids_t resctrl_ids)
{
	seq_printf(sf, "%lld\n", FIELD_GET(RMID_FIELD, resctrl_ids));

	return 0;
}

static struct resctrl_ctrlfileinfo files[] = {
	{
		.name	= "ctrl_hw_id",
		.show	= closid_show,
		.flags	= RESCTRL_CTRLMON_FILE | RESCTRL_MON_FILE,
	},
	{
		.name	= "mon_hw_id",
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
