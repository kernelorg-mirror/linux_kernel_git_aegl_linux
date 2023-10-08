// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2023 Intel Corporation. */

#include <asm/cpufeatures.h>
#include <linux/kthread.h>
#include <linux/delay.h>

#include "../../internal.h"
#include "rdt.h"

#define MBM_POLL_DELAY		1000	// milliseconds

static int max_threshold_occupancy;
static int mbm_width = 24;
static char *mon_features;
static int num_rmids;
static int upscale;
static unsigned int resctrl_rmid_realloc_limit;
static u64 llc_busy_threshold;

/* Count of clients following each h/w event */
static int active_events[EV_ARRAY_SIZE];

static struct resctrl_resource monitor;

struct rmid {
	struct list_head	list;
	struct list_head	child_list;
	bool			is_parent;
};

/*
 * Hardware counter for memory bandwidth may wrap periodically.
 * Keep track of the total traffic measured.
 * chunks:	: Multiply by "upscale" to convert to bytes
 * prev_msr:	: Previous value read from IA32_QM_CTR
 * prev_jiffies	: Timestamp of previous read
 * rate		: chunks/sec in previous poll interval
 */
struct mbm_event_state {
	u64	chunks;
	u64	prev_msr;
	u64	prev_jiffies;
	u64	rate;
	bool	init;
};

/* Need separate state for local and total memory bandwidth */
struct arch_mbm_state {
	struct mbm_event_state state[2];
};

struct mydomain {
	RESCTRL_DOMAIN_HEADER;
	int			cpu;
	struct task_struct	*kthread;
	struct arch_mbm_state	state[];
};

struct rmid_info {
	struct mydomain	*mydomain;
	u32		eventmap;
};

static LIST_HEAD(active_rmids);
static LIST_HEAD(free_rmids);
static struct rmid *rmid_array;

static u64 wrap(u64 old, u64 new)
{
	u64 shift = 64 - mbm_width, chunks;

	chunks = (new << shift) - (old << shift);

	return chunks >> shift;
}

static void update_rmids(void *info)
{
	struct rmid_info *ri = info;
	struct mbm_event_state *s;
	u64 addchunks, now;
	u32 map, event;
	struct rmid *r;

	list_for_each_entry(r, &active_rmids, list) {
		u64 msr, rmid = r - rmid_array;

		for (map = ri->eventmap; map; map &= ~BIT(event)) {
			event = __ffs(map);

			if (event == EV_TOT)
				s = &ri->mydomain->state[rmid].state[0];
			else
				s = &ri->mydomain->state[rmid].state[1];
			wrmsrl(MSR_IA32_QM_EVTSEL, (rmid << 32) | event);
			rdmsrl(MSR_IA32_QM_CTR, msr);
			now = jiffies;
			addchunks = wrap(s->prev_msr, msr);
			if (s->init) {
				s->chunks += addchunks;
				s->rate = addchunks * HZ;
				do_div(s->rate, (now - s->prev_jiffies));
			} else {
				s->chunks = 0;
				s->rate = 0;
				s->init = true;
			}
			s->prev_jiffies = now;
			s->prev_msr = msr;
		}
	}
}

static bool mbm_is_active(void)
{
	return (active_events[EV_TOT] + active_events[EV_LOC]) > 0;
}

static int mbm_poll(void *v)
{
	int cpu = raw_smp_processor_id();
	struct mydomain *m = v;
	struct rmid_info ri;

	ri.mydomain = m;

	while (!kthread_should_stop()) {
		mutex_lock(&resctrl_mutex);

		/* old CPU went offline? */
		if (cpu != raw_smp_processor_id()) {
			mutex_unlock(&resctrl_mutex);
			break;
		}

		ri.eventmap = 0;
		if (active_events[EV_TOT])
			ri.eventmap |= BIT(EV_TOT);
		if (active_events[EV_LOC])
			ri.eventmap |= BIT(EV_LOC);

		if (ri.eventmap) {
			update_rmids(&ri);
		} else {
			m->cpu = -1;
			mutex_unlock(&resctrl_mutex);
			break;
		}

		mutex_unlock(&resctrl_mutex);

		msleep(MBM_POLL_DELAY);
	}

	return 0;
}

static void init_poll_one_domain(struct mydomain *m)
{
	lockdep_assert_held(&resctrl_mutex);
	if (m->cpu != -1)
		return;

	m->cpu = cpumask_any(&m->cpu_mask);
	m->kthread = kthread_create_on_cpu(mbm_poll, m, m->cpu, "resctrl mbm %d");
	wake_up_process(m->kthread);
}

static void init_rmid_polling(void)
{
	struct mydomain *m;

	list_for_each_entry(m, &monitor.domains, list)
		init_poll_one_domain(m);
}

void arch_add_monitor(int mon_event)
{
	switch (mon_event) {
	case EV_LOCRATE:
		mon_event = EV_LOC;
		break;
	case EV_TOTRATE:
		mon_event = EV_TOT;
		break;
	}

	active_events[mon_event]++;

	if (mon_event == EV_TOT || mon_event == EV_LOC)
		if (mbm_is_active())
			init_rmid_polling();
}

void arch_del_monitor(int mon_event)
{
	switch (mon_event) {
	case EV_LOCRATE:
		mon_event = EV_LOC;
		break;
	case EV_TOTRATE:
		mon_event = EV_TOT;
		break;
	}

	active_events[mon_event]--;
}

int rmid_alloc(int prmid)
{
	struct mydomain *m;
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

	list_for_each_entry(m, &monitor.domains, list) {
		m->state[r - rmid_array].state[0].init = false;
		m->state[r - rmid_array].state[1].init = false;
	}

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

static void domain_update(struct resctrl_resource *r, int what, int cpu, void *domain)
{
	struct mydomain *m = domain;

	if (what == RESCTRL_DOMAIN_DELETE) {
		/* Last CPU in domain going offline, stop polling */
		m->kthread = NULL;
	} else if (what == RESCTRL_DOMAIN_DELETE_CPU && cpu == m->cpu) {
		/* Polling CPU for this domain going offline, pick another */
		m->kthread = NULL;
		m->cpu = -1;
		init_poll_one_domain(m);
	} else if (what == RESCTRL_DOMAIN_ADD) {
		/* New domain online, start polling */
		m->cpu = -1;
		init_poll_one_domain(m);
	}
}

static ssize_t max_threshold_occupancy_write(char *buf, size_t nbytes)
{
	unsigned int bytes;
	int ret;

	ret = kstrtouint(buf, 0, &bytes);
	if (ret)
		return ret;

	if (bytes > resctrl_rmid_realloc_limit)
		return -EINVAL;

	llc_busy_threshold = bytes / upscale;
	max_threshold_occupancy = llc_busy_threshold * upscale;

	return nbytes;
}

RESCTRL_FILE_DEF(max_threshold_occupancy, "%d\n")
RESCTRL_FILE_DEF(mon_features, "%s")
RESCTRL_FILE_DEF(num_rmids, "%d\n")

static struct resctrl_fileinfo monitor_files[] = {
	{
		.name	= "max_threshold_occupancy",
		.show	= max_threshold_occupancy_show,
		.write	= max_threshold_occupancy_write,
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

static void mount(bool mounted)
{
	struct mydomain *m;

	if (mounted) {
		list_for_each_entry(m, &monitor.domains, list) {
			m->state[0].state[0].init = false;
			m->state[0].state[1].init = false;
		}
	}
}

static struct resctrl_resource monitor = {
	.scope		= RESCTRL_L3CACHE,
	.domain_size	= sizeof(struct mydomain),
	.domains	= LIST_HEAD_INIT(monitor.domains),
	.domain_update	= domain_update,
	.domain_update_flag = true,
	.mount		= mount,
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
	if (boot_cpu_data.x86_vendor == X86_VENDOR_AMD) {
		if (eax & 0xff)
			mbm_width += 20;
	} else {
		mbm_width += eax & 0xff;
	}
	upscale = ebx;
	num_rmids = ecx + 1;

	monitor.domain_size += num_rmids * sizeof(struct arch_mbm_state);

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
