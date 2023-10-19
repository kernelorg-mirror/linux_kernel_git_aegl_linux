// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2023 Intel Corporation. */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/seq_file.h>
#include <linux/mod_devicetable.h>

#include <asm/cpufeatures.h>
#include <asm/cpu_device_id.h>

#include "../../internal.h"
#include "rdt.h"

static int mon_show(struct seq_file *sf, int domain_id, resctrl_ids_t resctrl_ids)
{
	int rmid = FIELD_GET(RMID_FIELD, resctrl_ids);

	seq_printf(sf, "%llu\n", rdt_rmid_read(domain_id, rmid, EV_TOT));

	return 0;
}

static void domain_update(struct resctrl_resource *r, int what, int cpu, void *domain)
{
}

static struct resctrl_resource mon = {
	.scope		= RESCTRL_L3CACHE,
	.domain_size	= sizeof(struct resctrl_domain),
	.domains	= LIST_HEAD_INIT(mon.domains),
	.domain_update	= domain_update,
	.mon_domain_dir	= "mon_L3_%02d",
	.mon_domain_file = "mbm_total_bytes",
	.mon_event	= EV_TOT,
	.mon_show	= mon_show,
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
