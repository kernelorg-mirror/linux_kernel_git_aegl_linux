// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2023 Intel Corporation. */

#include <asm/cpufeatures.h>
#include <linux/kthread.h>
#include <linux/delay.h>

#include "../../internal.h"
#include "rdt.h"

static int max_threshold_occupancy;
static char *mon_features;
static int num_rmids;
static int upscale;
static unsigned int resctrl_rmid_realloc_limit;
static u64 llc_busy_threshold;

struct rmid {
	struct list_head	list;
	struct list_head	child_list;
	bool			is_parent;
};

static LIST_HEAD(active_rmids);
static LIST_HEAD(free_rmids);
static struct rmid *rmid_array;

int rmid_alloc(int prmid)
{
	struct rmid *r;

	if (!num_rmids)
		return 0;

	if (list_empty(&free_rmids))
		return -ENOSPC;

	r = list_first_entry(&free_rmids, struct rmid, list);

	if (prmid < 0) {
		r->is_parent = true;
		INIT_LIST_HEAD(&r->child_list);
	} else {
		r->is_parent = false;
		list_add(&r->child_list, &rmid_array[prmid].child_list);
	}

	list_move(&r->list, &active_rmids);

	return r - rmid_array;
}

void rmid_free(int rmid)
{
	struct rmid *r = &rmid_array[rmid];

	if (!num_rmids)
		return;

	list_move_tail(&r->list, &free_rmids);

	if (r->is_parent)
		WARN_ON(!list_empty(&r->child_list));
	else
		list_del(&r->child_list);
}

void rmid_reparent(int rmid, int prmid)
{
	struct rmid *r = &rmid_array[rmid];
	struct rmid *pr = &rmid_array[prmid];

	list_move(&r->child_list, &pr->child_list);
}

RESCTRL_FILE_DEF(max_threshold_occupancy, "%d\n")
RESCTRL_FILE_DEF(mon_features, "%s")
RESCTRL_FILE_DEF(num_rmids, "%d\n")

static struct resctrl_fileinfo monitor_files[] = {
	{
		.name	= "max_threshold_occupancy",
		.show	= max_threshold_occupancy_show,
	},
	{
		.name	= "mon_features",
		.show	= mon_features_show,
	},
	{
		.name	= "num_rmids",
		.show	= num_rmids_show,
	},
	{ }
};

static struct resctrl_resource monitor = {
	.infodir	= "L3_MON",
	.infofiles	= monitor_files,
};

static void add_feature(char *feature)
{
	char *tmp;

	tmp = kasprintf(GFP_KERNEL, "%s%s\n", mon_features ?: "", feature);
	kfree(mon_features);
	mon_features = tmp;
}

static int __init rdt_monitor_init(void)
{
	u32 eax, ebx, ecx, edx;

	if (!boot_cpu_has(X86_FEATURE_CQM) || !boot_cpu_has(X86_FEATURE_CQM_LLC))
		return -ENODEV;

	if (boot_cpu_has(X86_FEATURE_CQM_OCCUP_LLC))
		add_feature("llc_occupancy");
	if (boot_cpu_has(X86_FEATURE_CQM_MBM_TOTAL))
		add_feature("mbm_total_bytes");
	if (boot_cpu_has(X86_FEATURE_CQM_MBM_LOCAL))
		add_feature("mbm_local_bytes");

	cpuid_count(0xf, 1, &eax, &ebx, &ecx, &edx);
	upscale = ebx;
	num_rmids = ecx + 1;

	/*
	 * A reasonable upper limit on the max threshold is the number
	 * of lines tagged per RMID if all RMIDs have the same number of
	 * lines tagged in the LLC.
	 *
	 * For a 35MB LLC and 56 RMIDs, this is ~1.8% of the LLC.
	 */
	resctrl_rmid_realloc_limit = boot_cpu_data.x86_cache_size * 1024;
	llc_busy_threshold = (resctrl_rmid_realloc_limit / num_rmids) / upscale;
	max_threshold_occupancy = llc_busy_threshold * upscale;

	rmid_array = kzalloc(sizeof(*rmid_array) * num_rmids, GFP_KERNEL);
	if (!rmid_array)
		return -ENOMEM;

	rmid_array[0].is_parent = true;
	INIT_LIST_HEAD(&rmid_array[0].child_list);
	list_add(&rmid_array[0].list, &active_rmids);

	for (int i = 1; i < num_rmids; i++)
		list_add_tail(&rmid_array[i].list, &free_rmids);

	resctrl_register_resource(&monitor);

	return 0;
}

late_initcall(rdt_monitor_init);

MODULE_AUTHOR("Tony Luck <tony.luck@intel.com>");
MODULE_IMPORT_NS(RESCTRL);
MODULE_LICENSE("GPL");
