// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2023 Intel Corporation. */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/resctrl.h>
#include <linux/seq_file.h>

#include <asm/cpufeatures.h>

#include "rdt.h"

#ifndef EVENT
#error "Need definition of which EVENT this module tracks"
#endif

static int mon_show(struct seq_file *sf, int domain_id, u64 resctrl_ids)
{
	seq_printf(sf, "%llu\n", rdt_rmid_read(domain_id, resctrl_ids & 0xffff, EVENT));

	return 0;
}

static void domain_update(struct resctrl_resource *r, int what, int cpu, struct resctrl_domain *d)
{
}

static struct resctrl_resource mon = {
	.name		= "L3",
	.archtag	= MSR_IA32_QM_EVTSEL,
	.type		= RESCTRL_MONITOR,
	.scope		= RESCTRL_L3CACHE,
	.domain_size	= sizeof(struct resctrl_domain),
	.domains	= LIST_HEAD_INIT(mon.domains),
	.domain_update	= domain_update,
	.mon_domain_dir	= "mon_L3_%02d",
#if EVENT == EV_LLC
	.mon_domain_file= "llc_occupancy",
#elif EVENT == EV_TOT
	.mon_domain_file= "mbm_total_bytes",
#elif EVENT == EV_LOC
	.mon_domain_file= "mbm_local_bytes",
#elif EVENT == EV_TOTRATE
	.mon_domain_file= "mbm_total_rate",
#elif EVENT == EV_LOCRATE
	.mon_domain_file= "mbm_local_rate",
#else
#error "Unknown EVENT type"
#endif
	.mon_show	= mon_show,
	.mon_event	= EVENT,
};

static int rdt_monitor_init(void)
{
	u32 eax, ebx, ecx, edx;
	int bit;

	switch (EVENT) {
	case EV_LLC: case EV_TOT: case EV_LOC:
		bit = EVENT - 1;
		break;
	case EV_TOTRATE:
		bit = EV_TOT - 1;
		break;
	case EV_LOCRATE:
		bit = EV_LOC - 1;
		break;
	}
	if (!boot_cpu_has(X86_FEATURE_CQM))
		return -ENODEV;

	cpuid_count(0xf, 0, &eax, &ebx, &ecx, &edx);
	if (!(edx & BIT(1)))
		return -ENODEV;

	cpuid_count(0xf, 1, &eax, &ebx, &ecx, &edx);
	if (!(edx & BIT(bit)))
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

MODULE_LICENSE("GPL");
