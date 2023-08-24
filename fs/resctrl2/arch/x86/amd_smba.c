// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2023 Intel Corporation. */

/*
 * AMD - Slow Memory Bandwidth Allocation
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/resctrl.h>
#include <linux/seq_file.h>

#include "rdt.h"

#undef pr_fmt
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#define	MAX_MBA_BW_AMD	0x800

struct throttle_values {
	u64     now;
	u64     staged;
	bool    need_update;
};

struct mydomain {
	RESCTRL_DOMAIN_HEADER;
	int			max_throttle;
	struct throttle_values	throttle_values[];
};

static struct resctrl_resource smba;

static void show(struct resctrl_resource *r, struct seq_file *sf, u64 resctrl_ids)
{
	int closid = (resctrl_ids >> 32);
	struct throttle_values *tvalues;
	struct mydomain *m;
	char *sep = "";

	list_for_each_entry(m, &r->domains, list) {
		tvalues = m->throttle_values;
		seq_printf(sf, "%s%d=%lld", sep, m->id, tvalues[closid].now);
		sep = ";";
	}
	seq_puts(sf, "\n");
}

static void resetstaging(struct resctrl_resource *r, u64 resctrl_ids)
{
	int closid = (resctrl_ids >> 32);
	struct throttle_values *tvalues;
	struct mydomain *m;

	list_for_each_entry(m, &r->domains, list) {
		tvalues = m->throttle_values;
		tvalues[closid].need_update = false;
	}
}

static bool validate_throttle(struct mydomain *m, char *buf, struct throttle_values *c)
{
	unsigned long val;
	int ret;

	ret = kstrtoul(buf, 10, &val);
	if (ret) {
		resctrl_last_cmd_printf("Non-decimal character in the value %s\n", buf);
		return false;
	}

	/* User didn't change this value */
	if (val == c->now)
		return true;

	if (val > MAX_MBA_BW_AMD) {
		resctrl_last_cmd_puts("Throttle value out of range\n");
		return false;
	}

	c->need_update = true;
	c->staged = val;

	return true;
}

static int parse(struct resctrl_resource *r, char *line, u64 resctrl_ids)
{
	int closid = (resctrl_ids >> 32);
	struct throttle_values *tvalues;
	char *dom = NULL, *id;
	unsigned long dom_id;
	struct mydomain *m;

next:
	if (!line || line[0] == '\0')
		return 0;
	dom = strsep(&line, ";");
	id = strsep(&dom, "=");
	id = strim(id);
	if (!dom || kstrtoul(id, 10, &dom_id)) {
		resctrl_last_cmd_puts("Missing '=' or non-numeric domain\n");
		return -EINVAL;
	}
	dom = strim(dom);
	list_for_each_entry(m, &r->domains, list) {
		if (m->id != dom_id)
			continue;
		tvalues = m->throttle_values;
		if (!validate_throttle(m, dom, tvalues + closid))
			return -EINVAL;
		goto next;
	}
	return -EINVAL;
}

struct rdt_msr_info {
	int	msr_base;
	struct throttle_values *tvalues;
};

static void update_msrs(void *info)
{
	struct rdt_msr_info *mi = info;

	for (int i = 0; i < smba.num_alloc_ids; i++) {
		if (mi->tvalues[i].need_update) {
			mi->tvalues[i].now = mi->tvalues[i].staged;
			mi->tvalues[i].need_update = false;
			wrmsrl(mi->msr_base + i, mi->tvalues[i].now);
		}
	}
}

static void applychanges(struct resctrl_resource *r, u64 resctrl_ids)
{
	int closid = (resctrl_ids >> 32);
	struct throttle_values *tvalues;
	struct rdt_msr_info mi;
	struct mydomain *m;

	list_for_each_entry(m, &r->domains, list) {
		tvalues = m->throttle_values;
		if (!tvalues[closid].need_update)
			continue;
		mi.msr_base = r->archtag;
		mi.tvalues = tvalues;
		smp_call_function_single(cpumask_first(&m->cpu_mask), update_msrs, &mi, 1);
	}
}

/*
 * On domain discovery (duing module load, or CPU hotplug) set
 * all controls to allow full access to all of cache. Ditto on
 * module unload or domain removal.
 */
static void domain_update(struct resctrl_resource *r, int what, int cpu, void *domain)
{
	struct throttle_values *tvalues;
	struct mydomain *m = domain;
	struct rdt_msr_info mi;

	tvalues = m->throttle_values;
	if (what == RESCTRL_DOMAIN_ADD || what == RESCTRL_DOMAIN_DELETE) {
		for (int i = 0; i < smba.num_alloc_ids; i++) {
			tvalues[i].staged = MAX_MBA_BW_AMD;
			tvalues[i].need_update = true;
		}
		mi.msr_base = r->archtag;
		mi.tvalues = tvalues;
		smp_call_function_single(cpu, update_msrs, &mi, 1);
	}
}

static void reset(struct resctrl_resource *r)
{
	struct throttle_values *tvalues;
	struct rdt_msr_info mi;
	struct mydomain *m;

	list_for_each_entry(m, &r->domains, list) {
		tvalues = m->throttle_values;

		for (int i = 0; i < smba.num_alloc_ids; i++) {
			tvalues[i].staged = MAX_MBA_BW_AMD;
			if (tvalues[i].staged != tvalues[i].now)
				tvalues[i].need_update = true;
		}
		mi.msr_base = r->archtag;
		mi.tvalues = tvalues;
		smp_call_function_single(cpumask_first(&m->cpu_mask), update_msrs, &mi, 1);
	}
}

static int bandwidth_gran = 1;
static int delay_linear = 1;
static int min_bandwidth = 0;
static int num_closids;
RESCTRL_FILE_DEF(bandwidth_gran, "%d\n")
RESCTRL_FILE_DEF(delay_linear, "%d\n")
RESCTRL_FILE_DEF(min_bandwidth, "%d\n")
RESCTRL_FILE_DEF(num_closids, "%d\n")

static struct resctrl_fileinfo smba_files[] = {
	{
		.name	= "bandwidth_gran",
		.show	= &bandwidth_gran_show,
	},
	{
		.name	= "delay_linear",
		.show	= &delay_linear_show,
	},
	{
		.name	= "min_bandwidth",
		.show	= &min_bandwidth_show,
	},
	{
		.name	= "num_closids",
		.show	= &num_closids_show,
	},
	{ }
};

static struct resctrl_resource smba = {
	.name		= "SMBA",
	.archtag	= MSR_IA32_SMBA_BW_BASE,
	.show		= show,
	.resetstaging	= resetstaging,
	.parse		= parse,
	.applychanges	= applychanges,
	.scope		= RESCTRL_L3CACHE,
	.domain_size	= sizeof(struct mydomain),
	.domains	= LIST_HEAD_INIT(smba.domains),
	.domain_update	= domain_update,
	.reset		= reset,
	.infodir	= "SMBA",
	.infofiles	= smba_files,
};

static int __init smba_init(void)
{
	u32 eax, ebx, ecx, edx;
	int ret;

	if (boot_cpu_data.x86_vendor != X86_VENDOR_AMD ||
	    !boot_cpu_has(X86_FEATURE_SMBA))
		return -ENODEV;

	cpuid_count(0x80000020, 2, &eax, &ebx, &ecx, &edx);
	num_closids = (edx & 0xffff) + 1;
	delay_linear = 1;

	smba.domain_size += num_closids * sizeof(struct throttle_values);
	smba.num_alloc_ids = num_closids;

	ret = resctrl_register_resource(&smba);
	return ret;
}

static void __exit smba_cleanup(void)
{
	resctrl_unregister_resource(&smba);
}

module_init(smba_init);
module_exit(smba_cleanup);

MODULE_AUTHOR("Tony Luck <tony.luck@intel.com>");
MODULE_IMPORT_NS(RESCTRL);
MODULE_LICENSE("GPL");
