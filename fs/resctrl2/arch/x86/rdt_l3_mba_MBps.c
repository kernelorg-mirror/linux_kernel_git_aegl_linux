// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2023 Intel Corporation. */

/*
 * X86 Resource Control Driver Memory Bandwidth Allocation with
 * feedback loop from local Memory Bandwidth Monitoring.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/resctrl.h>
#include <linux/seq_file.h>

#include "../../internal.h"

#include "rdt.h"

#undef pr_fmt
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#define MBA_MAX_MBPS U32_MAX

struct bandwidth_values {
	u64	now;
	u64	target_low, target_high;
	u64	staged;
	u64	throttle;
	bool	need_update;
};

struct mydomain {
	int			max_bandwidth;
	int			cpu;
	struct task_struct	*kthread;
	struct bandwidth_values	bandwidth_values[];
};

#define get_mydomain(d) ((struct mydomain *)(&d[1]))

static struct resctrl_resource mba;

static int bandwidth_gran, delay_linear, min_bandwidth, num_closids, max_throttle;

static void show(struct resctrl_resource *r, struct seq_file *m, u64 resctrl_ids)
{
	int closid = (resctrl_ids >> 32);
	struct resctrl_domain *d;
	struct bandwidth_values *tvalues;
	char *sep = "";

	list_for_each_entry(d, &r->domains, list) {
		tvalues = get_mydomain(d)->bandwidth_values;
		seq_printf(m, "%s%d=%lld", sep, d->id, tvalues[closid].now);
		sep = ";";
	}
	seq_puts(m, "\n");
}

static void resetstaging(struct resctrl_resource *r, u64 resctrl_ids)
{
	int closid = (resctrl_ids >> 32);
	struct resctrl_domain *d;
	struct bandwidth_values *tvalues;

	list_for_each_entry(d, &r->domains, list) {
		tvalues = get_mydomain(d)->bandwidth_values;
		tvalues[closid].need_update = false;
	}
}

static bool validate_bandwidth(struct resctrl_domain *d, char *buf, struct bandwidth_values *c)
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

	if (val > MBA_MAX_MBPS) {
		resctrl_last_cmd_puts("Bandwidth value out of range\n");
		return false;
	}

	c->need_update = true;
	c->staged = val;

	return true;
}

static int parse(struct resctrl_resource *r, char *line, u64 resctrl_ids)
{
	int closid = (resctrl_ids >> 32);
	struct bandwidth_values *tvalues;
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
		resctrl_last_cmd_puts("Missing '=' or non-numeric domain\n");
		return -EINVAL;
	}
	dom = strim(dom);
	list_for_each_entry(d, &r->domains, list) {
		if (d->id != dom_id)
			continue;
		tvalues = get_mydomain(d)->bandwidth_values;
		if (!validate_bandwidth(d, dom, tvalues + closid))
			return -EINVAL;
		goto next;
	}
	return -EINVAL;
}

struct rdt_msr_info {
	int	msr_base;
	struct bandwidth_values *tvalues;
};

static void update_msrs(void *info)
{
	struct rdt_msr_info *mi = info;

	for (int i = 0; i < mba.num_alloc_ids; i++) {
		if (mi->tvalues[i].need_update) {
			mi->tvalues[i].need_update = false;
			wrmsrl(mi->msr_base + i, mi->tvalues[i].throttle);
		}
	}
}

static void applychanges(struct resctrl_resource *r, u64 resctrl_ids)
{
	int closid = (resctrl_ids >> 32);
	struct resctrl_domain *d;
	struct bandwidth_values *tvalues;

	list_for_each_entry(d, &r->domains, list) {
		tvalues = get_mydomain(d)->bandwidth_values;
		if (tvalues[closid].need_update) {
			tvalues[closid].need_update = false;
			tvalues[closid].now = tvalues[closid].staged;
			tvalues[closid].target_low = tvalues[closid].now * (100 - bandwidth_gran);
			do_div(tvalues[closid].target_low, 100);
			tvalues[closid].target_high = tvalues[closid].now * (100 + bandwidth_gran);
			do_div(tvalues[closid].target_high, 100);
		}
	}
}

static void check_one(u64 resctrl_ids, void *v)
{
	struct bandwidth_values *tvalues;
	u64 rmid = resctrl_ids & 0xffff;
	u64 closid = resctrl_ids >> 32;
	struct resctrl_domain *d = v;
	struct mydomain *m;
	u64 got;

	m = get_mydomain(d);
	tvalues = (struct bandwidth_values *)(m + 1);

	got = rdt_rmid_read(d->id, rmid, EV_LOCRATE) >> 20;

	if (got > tvalues[closid].target_high && tvalues[closid].throttle < max_throttle) {
		tvalues[closid].throttle += bandwidth_gran;
		tvalues[closid].need_update = true;
	} else if (got < tvalues[closid].target_low && tvalues[closid].throttle > 0) {
		tvalues[closid].throttle -= bandwidth_gran;
		tvalues[closid].need_update = true;
	}
}

static int checkbw(void *v)
{
	struct bandwidth_values *tvalues;
	struct resctrl_domain *d = v;
	struct mydomain *m;
	struct rdt_msr_info mi;

	m = get_mydomain(d);
	tvalues = (struct bandwidth_values *)(m + 1);

	while (!kthread_should_stop()) {
		resctrl_ctrl_callback(check_one, v);

		mi.msr_base = mba.archtag;
		mi.tvalues = tvalues;
		update_msrs(&mi);

		msleep(1000);
	}

	return 0;
}

/*
 * On domain discovery (duing module load, or CPU hotplug) set
 * all controls to allow full access to all of cache. Ditto on
 * module unload or domain removal.
 */
static void domain_update(struct resctrl_resource *r, int what, int cpu, struct resctrl_domain *d)
{
	struct mydomain *m = get_mydomain(d);
	struct bandwidth_values *tvalues;
	struct rdt_msr_info mi;

	tvalues = m->bandwidth_values;
	if (what == RESCTRL_DOMAIN_ADD || what == RESCTRL_DOMAIN_DELETE) {
		for (int i = 0; i < mba.num_alloc_ids; i++) {
			tvalues[i].now = MBA_MAX_MBPS;
			tvalues[i].target_low = MBA_MAX_MBPS;
			tvalues[i].target_high = MBA_MAX_MBPS;
			tvalues[i].throttle = 0;
			tvalues[i].need_update = true;
		}
		mi.msr_base = r->archtag;
		mi.tvalues = tvalues;
		smp_call_function_single(cpu, update_msrs, &mi, 1);

		if (what == RESCTRL_DOMAIN_ADD) {
			m->cpu = cpumask_first(&d->cpu_mask);
			m->kthread = kthread_create_on_cpu(checkbw, d, m->cpu, "mba_MBps %d");
			wake_up_process(m->kthread);
		} else {
			if (m->kthread) {
				kthread_stop(m->kthread);
				m->kthread = NULL;
			}
		}
	} else if (what == RESCTRL_DOMAIN_DELETE_CPU && cpu == m->cpu) {
		if (m->kthread) {
			kthread_stop(m->kthread);
			m->kthread = NULL;
		}
		m->cpu = cpumask_first(&d->cpu_mask);
		m->kthread = kthread_create_on_cpu(checkbw, d, m->cpu, "mba_MBps %d");
		wake_up_process(m->kthread);
	}
}

static void reset(struct resctrl_resource *r)
{
	struct bandwidth_values *tvalues;
	struct resctrl_domain *d;
	struct rdt_msr_info mi;
	struct mydomain *m;

	list_for_each_entry(d, &r->domains, list) {
		m = get_mydomain(d);
		tvalues = m->bandwidth_values;

		for (int i = 0; i < mba.num_alloc_ids; i++) {
			tvalues[i].throttle = 0;
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

static struct resctrl_resource mon_mbm_local = {
	.archtag	= MSR_IA32_QM_EVTSEL,
	.type		= RESCTRL_MONITOR,
	.mon_event	= EV_LOC,
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
	max_throttle = (eax & 0xfff) + 1;
	bandwidth_gran = 100 - max_throttle;
	delay_linear = !!(ecx & BIT(2));
	num_closids = edx + 1;

	mba.domain_size += num_closids * sizeof(struct bandwidth_values);
	mba.num_alloc_ids = num_closids;

	ret = resctrl_register_resource(&mba);

	resctrl_register_resource(&mon_mbm_local);

	return ret;
}

static void __exit mba_cleanup(void)
{
	resctrl_unregister_resource(&mon_mbm_local);
	resctrl_unregister_resource(&mba);
}

module_init(mba_init);
module_exit(mba_cleanup);

MODULE_AUTHOR("Tony Luck <tony.luck@intel.com>");
MODULE_IMPORT_NS(RESCTRL);
MODULE_LICENSE("GPL");
