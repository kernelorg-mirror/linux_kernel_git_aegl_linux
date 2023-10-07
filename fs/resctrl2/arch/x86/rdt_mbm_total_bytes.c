// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2023 Intel Corporation. */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/resctrl.h>
#include <linux/seq_file.h>
#include <linux/mod_devicetable.h>

#include <asm/cpufeatures.h>
#include <asm/cpu_device_id.h>

#include "rdt.h"

static struct resctrl_resource mon = {
	.mon_event	= EV_TOT,
};

static const struct x86_cpu_id mon_feature[] = {
	X86_MATCH_FEATURE(X86_FEATURE_CQM_MBM_TOTAL, 0),
	{ }
};
MODULE_DEVICE_TABLE(x86cpu, mon_feature);

static int rdt_monitor_init(void)
{
	if (!boot_cpu_has(X86_FEATURE_CQM) || !x86_match_cpu(mon_feature))
		return -ENODEV;

	resctrl_register_resource(&mon);

	return 0;
}

static void rdt_monitor_exit(void)
{
	resctrl_unregister_resource(&mon);
}

module_init(rdt_monitor_init);
module_exit(rdt_monitor_exit);

MODULE_AUTHOR("Tony Luck <tony.luck@intel.com>");
MODULE_IMPORT_NS(RESCTRL);
MODULE_LICENSE("GPL");
