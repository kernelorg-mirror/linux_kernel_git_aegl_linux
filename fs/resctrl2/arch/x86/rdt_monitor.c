// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2023 Intel Corporation. */

#include <asm/cpufeatures.h>
#include <linux/kthread.h>
#include <linux/delay.h>

#include "../../internal.h"
#include "rdt.h"

#define MBM_POLL_DELAY	1000	// milliseconds

char *stpcpy(char *__restrict__ dest, const char *__restrict__ src);

struct rmid {
	struct list_head	list;
	struct list_head	child_list;
	bool			is_parent;
	u64			llc_busy_domains;
};

struct mbm_event_state {
	u64	chunks;
	u64	prev_msr;
	u64	prev_jiffies;
	u64	rate;
};

struct arch_mbm_state {
	struct mbm_event_state state[2];
};

struct mydomain {
	int			cpu;
	struct task_struct	*kthread;
	struct arch_mbm_state	state[];
};
#define get_mydomain(d) ((struct mydomain *)&d[1])

struct rmid_info {
	struct mydomain *mydomain;
	u32	eventmap;
	bool	init;
};

static LIST_HEAD(active_rmids);
static LIST_HEAD(free_rmids);
static LIST_HEAD(limbo_rmids);

static struct rmid *rmid_array;
static int num_rmids;
static int upscale;
static int max_threshold_occupancy;
static int mbm_width = 24;
static char mon_features[64];
static struct resctrl_resource monitor;
static int active_events[EV_MAX];

static void init_rmids(int mon_event);
static void update_rmids(void *info);
static bool rmid_polling;
static u64 llc_busy_threshold;
unsigned int resctrl_rmid_realloc_limit;

static void check_limbo(struct resctrl_domain *d)
{
	struct rmid *r, *tmp;

	list_for_each_entry_safe(r, tmp, &limbo_rmids, list) {
		u64 rmid = r - rmid_array;
		u64 chunks;

		if (!(r->llc_busy_domains & BIT(d->id)))
			continue;
		wrmsrl(MSR_IA32_QM_EVTSEL, (rmid << 32) | EV_LLC);
		rdmsrl(MSR_IA32_QM_CTR, chunks);

		if (chunks <= llc_busy_threshold) {
			r->llc_busy_domains &= ~BIT(d->id);
			if (!r->llc_busy_domains)
				list_move_tail(&r->list, &free_rmids);
		}
	}
}

static bool mbm_is_active(void)
{
	return (active_events[EV_TOT] + active_events[EV_LOC]) > 0;
}

static int mbm_poll(void *v)
{
	struct resctrl_domain *d = v;
	struct rmid_info ri;

	ri.mydomain = get_mydomain(d);
	ri.init = false;

	while (rmid_polling && !kthread_should_stop()) {

		msleep(MBM_POLL_DELAY);

		mutex_lock(&resctrl_mutex);

		ri.eventmap = 0;
		if (active_events[EV_TOT])
			ri.eventmap |= BIT(EV_TOT);
		if (active_events[EV_LOC])
			ri.eventmap |= BIT(EV_LOC);

		if (ri.eventmap)
			update_rmids(&ri);
		if (!list_empty(&limbo_rmids))
			check_limbo(d);

		if (!ri.eventmap && list_empty(&limbo_rmids)) {
			rmid_polling = false;
		}

		mutex_unlock(&resctrl_mutex);

	}

	return 0;
}

static void init_poll_one_domain(struct resctrl_domain *d)
{
	struct mydomain *m;

	m = get_mydomain(d);
	m->cpu = cpumask_any(&d->cpu_mask);
	m->kthread = kthread_create_on_cpu(mbm_poll, d, m->cpu, "resctrl mbm %d");
	wake_up_process(m->kthread);
}

static void init_rmid_polling(void)
{
	struct resctrl_domain *d;

	rmid_polling = true;
	list_for_each_entry(d, &monitor.domains, list)
		init_poll_one_domain(d);
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

	if (mon_event == EV_TOT || mon_event == EV_LOC) {
		if (active_events[mon_event] == 1)
			init_rmids(mon_event);
		if (!rmid_polling && mbm_is_active())
			init_rmid_polling();
	}
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
	struct rmid *r;

	if (!num_rmids)
		return 0;

	if (list_empty(&free_rmids))
		return list_empty(&limbo_rmids) ? -ENOSPC : -EBUSY;

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
	struct resctrl_domain *d;

	if (!num_rmids)
		return;

	if (active_events[EV_LLC]) {
		list_for_each_entry(d, &monitor.domains, list)
			r->llc_busy_domains |= BIT(d->id);
		list_move_tail(&r->list, &limbo_rmids);
		if (!rmid_polling)
			init_rmid_polling();
	} else {
		list_move_tail(&r->list, &free_rmids);
	}
	if (r->is_parent)
		WARN_ON(!list_empty(&r->child_list));
	else
		list_del(&r->child_list);
}

static u64 wrap(u64 old, u64 new)
{
	u64 shift = 64 - mbm_width, chunks;

	chunks = (new << shift) - (old << shift);

	return chunks >> shift;
}

static u64 adjust(struct mydomain *m, u64 rmid, u64 event, u64 chunks)
{
	struct mbm_event_state *s;
	u64 rawchunks;


	switch (event) {
	case EV_LLC:
		rawchunks = chunks;
		break;
	case EV_TOT:
		s = &m->state[rmid].state[0];
		rawchunks = get_corrected_mbm_count(rmid, s->chunks + wrap(s->prev_msr, chunks));
		break;
	case EV_LOC:
		s = &m->state[rmid].state[1];
		rawchunks = get_corrected_mbm_count(rmid, s->chunks + wrap(s->prev_msr, chunks));
		break;
	case EV_TOTRATE:
		s = &m->state[rmid].state[0];
		rawchunks = get_corrected_mbm_count(rmid, s->rate);
		break;
	case EV_LOCRATE:
		s = &m->state[rmid].state[0];
		rawchunks = get_corrected_mbm_count(rmid, s->rate);
		break;
	}
	return rawchunks;
}

struct rrmid_info {
	struct resctrl_domain	*domain;
	u64			rmid;
	u64			event;
	u64			chunks;
};

static void __rdt_rmid_read(void *info)
{
	struct rrmid_info *rr = info;
	struct rmid *cr, *r;
	struct mydomain *m;
	u64 chunks;

	m = get_mydomain(rr->domain);

	if (rr->event <= EV_LOC) {
		wrmsrl(MSR_IA32_QM_EVTSEL, (rr->rmid << 32) | rr->event);
		rdmsrl(MSR_IA32_QM_CTR, chunks);
	} else {
		chunks = 0;
	}

	rr->chunks = adjust(m, rr->rmid, rr->event, chunks);

	r = &rmid_array[rr->rmid];
	if (r->is_parent && !list_empty(&r->child_list)) {
		list_for_each_entry(cr, &r->child_list, child_list) {
			u64 crmid = cr - rmid_array;

			if (rr->event <= EV_LOC) {
				wrmsrl(MSR_IA32_QM_EVTSEL, (crmid << 32) | rr->event);
				rdmsrl(MSR_IA32_QM_CTR, chunks);
			} else {
				chunks = 0;
			}

			rr->chunks += adjust(m, crmid, rr->event, chunks);
		}
	}
}

u64 rdt_rmid_read(int domain_id, int rmid, int event)
{
	struct resctrl_domain *d;
	struct rrmid_info rr;
	struct mydomain *m;

	list_for_each_entry(d, &monitor.domains, list)
		if (d->id == domain_id)
			goto found;
	return ~0ull;
found:
	m = get_mydomain(d);

	rr.domain = d;
	rr.rmid = rmid;
	rr.event = event;

	if (event <= EV_LOC)
		smp_call_function_any(&d->cpu_mask, __rdt_rmid_read, &rr, 1);
	else
		__rdt_rmid_read(&rr);

	return rr.chunks * upscale;
}
EXPORT_SYMBOL_GPL(rdt_rmid_read);

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
			if (ri->init) {
				s->chunks = 0;
				s->rate = 0;
			} else {
				s->chunks += addchunks;
				s->rate = addchunks * HZ / (now - s->prev_jiffies);
			}
			s->prev_jiffies = now;
			s->prev_msr = msr;
		}
	}
}

static void init_rmids(int mon_event)
{
	struct resctrl_domain *d;
	struct rmid_info ri;

	ri.init = true;

	list_for_each_entry(d, &monitor.domains, list) {
		ri.mydomain = get_mydomain(d);
		ri.eventmap = BIT(mon_event);
		smp_call_function_any(&d->cpu_mask, update_rmids, &ri, 1);
	}
}

static void domain_update(struct resctrl_resource *r, int what, int cpu, struct resctrl_domain *d)
{
	struct mydomain *m = get_mydomain(d);

	if (what == RESCTRL_DOMAIN_DELETE) {
		kthread_stop(m->kthread);
	} else if (what == RESCTRL_DOMAIN_DELETE_CPU && cpu == m->cpu) {
		kthread_stop(m->kthread);
		init_poll_one_domain(d);
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
		.name 	= "max_threshold_occupancy",
		.show 	= max_threshold_occupancy_show,
		.write 	= max_threshold_occupancy_write,
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
	.name		= "L3",
	.archtag	= MSR_IA32_QM_EVTSEL,
	.type		= RESCTRL_MONITOR,
	.scope		= RESCTRL_L3CACHE,
	.domain_size	= sizeof(struct resctrl_domain),
	.domains	= LIST_HEAD_INIT(monitor.domains),
	.domain_update	= domain_update,
	.infodir	= "L3_MON",
	.infofiles	= monitor_files,
};

static int __init rdt_monitor_init(void)
{
	u32 eax, ebx, ecx, edx;
	char *s;

	if (!boot_cpu_has(X86_FEATURE_CQM))
		return -ENODEV;

	cpuid_count(0xf, 0, &eax, &ebx, &ecx, &edx);
	if (!(edx & BIT(1)))
		return -ENODEV;

	cpuid_count(0xf, 1, &eax, &ebx, &ecx, &edx);
	mbm_width += eax & 0xff;
	upscale = ebx;
	num_rmids = ecx + 1;
	rdt_mbm_apply_quirk(num_rmids);

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

	s = mon_features;
	if (edx & BIT(0))
		s = stpcpy(s, "llc_occupancy\n");
	if (edx & BIT(1))
		s = stpcpy(s, "mbm_total_bytes\n");
	if (edx & BIT(2))
		s = stpcpy(s, "mbm_local_bytes\n");

	rmid_array = kzalloc(sizeof *rmid_array * num_rmids, GFP_KERNEL);
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
