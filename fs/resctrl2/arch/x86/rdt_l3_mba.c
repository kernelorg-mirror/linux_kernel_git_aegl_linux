// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2023 Intel Corporation. */

/*
 *  X86 Resource Control Driver For L2 and L3 cache allocation
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/resctrl.h>
#include <linux/seq_file.h>

#include "rdt.h"

#undef pr_fmt
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

struct throttle_values {
	u64	now;
	u64	staged;
	bool	need_update;
};

struct mydomain {
	int			max_throttle;
	struct throttle_values	throttle_values[];
};
#define get_mydomain(d) ((struct mydomain *)(&d[1]))

static struct resctrl_resource mba;

static int bandwidth_gran, delay_linear, min_bandwidth, num_closids;

static void show(struct resctrl_resource *r, struct seq_file *m, u64 resctrl_ids)
{
	int closid = (resctrl_ids >> 32);
	struct resctrl_domain *d;
	struct throttle_values *tvalues;
	char *sep = "";

	list_for_each_entry(d, &r->domains, list) {
		tvalues = get_mydomain(d)->throttle_values;
		seq_printf(m, "%s%d=%lld", sep, d->id, tvalues[closid].now);
		sep = ";";
	}
	seq_puts(m, "\n");
}

static void resetstaging(struct resctrl_resource *r, u64 resctrl_ids)
{
	int closid = (resctrl_ids >> 32);
	struct resctrl_domain *d;
	struct throttle_values *tvalues;

	list_for_each_entry(d, &r->domains, list) {
		tvalues = get_mydomain(d)->throttle_values;
		tvalues[closid].need_update = false;
	}
}

static bool validate_throttle(struct resctrl_domain *d, char *buf, struct throttle_values *c)
{
	unsigned long val;
	struct mydomain *m = get_mydomain(d);
	int ret;

	ret = kstrtoul(buf, 10, &val);
	if (ret) {
		// rdt_last_cmd_printf("Non-decimal character in the value %s\n", buf);
		return false;
	}

	/* User didn't change this value */
	if (val == c->now)
		return true;

	if (val > m->max_throttle) {
		// rdt_last_cmd_puts("Throttle value out of range\n");
		return false;
	}
	if (val % bandwidth_gran) {
		// rdt_last_cmd_printf("Throttle must be multiple of %lld\n", bandwidth_gran);
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
	struct resctrl_domain *d;
	unsigned long dom_id;

next:
	if (!line || line[0] == '\0')
		return 0;
	dom = strsep(&line, ";");
	id = strsep(&dom, "=");
	id = strim(id);
	if (!dom || kstrtoul(id, 10, &dom_id)) {
		// rdt_last_cmd_puts("Missing '=' or non-numeric domain\n");
		return -EINVAL;
	}
	dom = strim(dom);
	list_for_each_entry(d, &r->domains, list) {
		if (d->id != dom_id)
			continue;
		tvalues = get_mydomain(d)->throttle_values;
		if (!validate_throttle(d, dom, tvalues + closid))
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

	for (int i = 0; i < mba.num_alloc_ids; i++) {
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
	struct resctrl_domain *d;
	struct throttle_values *tvalues;
	struct rdt_msr_info mi;

	list_for_each_entry(d, &r->domains, list) {
		tvalues = get_mydomain(d)->throttle_values;
		if (!tvalues[closid].need_update)
			continue;
		mi.msr_base = r->archtag;
		mi.tvalues = tvalues;
		smp_call_function_single(cpumask_first(&d->cpu_mask), update_msrs, &mi, 1);
	}
}

/*
 * On domain discovery (duing module load, or CPU hotplug) set
 * all controls to allow full access to all of cache. Ditto on
 * module unload or domain removal.
 */
static void domain_update(struct resctrl_resource *r, int what, int cpu, struct resctrl_domain *d)
{
	struct mydomain *m = get_mydomain(d);
	struct throttle_values *tvalues;
	unsigned int eax, ebx, ecx, edx;
	struct rdt_msr_info mi;

	tvalues = m->throttle_values;
	if (what == RESCTRL_DOMAIN_ADD || what == RESCTRL_DOMAIN_DELETE) {
		cpuid_count(0x10, 3, &eax, &ebx, &ecx, &edx);
		m->max_throttle = (eax & 0xfff) + 1;
		bandwidth_gran = 100 - m->max_throttle;
		min_bandwidth = 100 - m->max_throttle;
		for (int i = 0; i < mba.num_alloc_ids; i++) {
			tvalues[i].staged = 0;
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
	struct resctrl_domain *d;
	struct rdt_msr_info mi;
	struct mydomain *m;

	list_for_each_entry(d, &r->domains, list) {
		m = get_mydomain(d);
		tvalues = m->throttle_values;

		for (int i = 0; i < mba.num_alloc_ids; i++) {
			tvalues[i].staged = 0;
			if (tvalues[i].staged != tvalues[i].now)
				tvalues[i].need_update = true;
		}
		mi.msr_base = r->archtag;
		mi.tvalues = tvalues;
		smp_call_function_single(cpumask_first(&d->cpu_mask), update_msrs, &mi, 1);
	}
}

RESCTRL_FILE_DEF(bandwidth_gran, "%d\n")
RESCTRL_FILE_DEF(delay_linear, "%d\n")
RESCTRL_FILE_DEF(min_bandwidth, "%d\n")
RESCTRL_FILE_DEF(num_closids, "%d\n")

static struct resctrl_fileinfo mb_files[] = {
	{ .name = "bandwidth_gran", .ops = &bandwidth_gran_ops },
	{ .name = "delay_linear", .ops = &delay_linear_ops },
	{ .name = "min_bandwidth", .ops = &min_bandwidth_ops },
	{ .name = "num_closids", .ops = &num_closids_ops },
	{ }
};

static struct resctrl_resource mba = {
	.name		= "MB",
	.archtag	= MSR_IA32_MBA_THRTL_BASE,
	.type		= RESCTRL_CONTROL,
	.show		= show,
	.resetstaging	= resetstaging,
	.parse		= parse,
	.applychanges	= applychanges,
	.scope		= RESCTRL_L3CACHE,
	.domain_size	= sizeof(struct resctrl_domain) + sizeof(struct mydomain),
	.domains	= LIST_HEAD_INIT(mba.domains),
	.domain_update	= domain_update,
	.reset		= reset,
	.infodir	= "MB",
	.infofiles	= mb_files,
};

static int __init mba_init(void)
{
	unsigned int eax, ebx, ecx, edx, mba_features;
	int ret;

	if (!boot_cpu_has(X86_FEATURE_RDT_A)) {
		pr_debug("No RDT allocation support\n");
		return -ENODEV;
	}

	mba_features = cpuid_ebx(0x10);

	if (!(mba_features & BIT(3))) {
		pr_debug("No RDT MBA allocation\n");
		return -ENODEV;
	}

	cpuid_count(0x10, 3, &eax, &ebx, &ecx, &edx);
	num_closids = edx + 1;
	delay_linear = !!(ecx & BIT(2));

	mba.domain_size += num_closids * sizeof(struct throttle_values);
	mba.num_alloc_ids = num_closids;

	ret = resctrl_register_resource(&mba);
	return ret;
}

static void __exit mba_cleanup(void)
{
	resctrl_unregister_resource(&mba);
}

module_init(mba_init);
module_exit(mba_cleanup);

MODULE_LICENSE("GPL");
