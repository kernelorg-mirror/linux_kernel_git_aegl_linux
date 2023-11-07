// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2023 Intel Corporation. */

#include <linux/cacheinfo.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/mod_devicetable.h>

#include <asm/cpufeatures.h>
#include <asm/cpu_device_id.h>

#include "../../internal.h"
#include "rdt.h"

#define MBM_POLL_DELAY		1000	// milliseconds

int arch_snc_nodes_per_l3_cache = 1;
EXPORT_SYMBOL_GPL(arch_snc_nodes_per_l3_cache);

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
	u64			llc_busy_domains;
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
struct arch_rmid_state {
	struct mbm_event_state state[2];
};

struct mydomain {
	RESCTRL_DOMAIN_HEADER;
	int			cpu;
	struct task_struct	*kthread;
	struct arch_rmid_state	rmids[];
};

struct rmid_info {
	struct mydomain	*mydomain;
	u32		eventmap;
};

static LIST_HEAD(active_rmids);
static LIST_HEAD(free_rmids);
static LIST_HEAD(limbo_rmids);

static struct rmid *rmid_array;

static u64 wrap(u64 old, u64 new)
{
	u64 shift = 64 - mbm_width, chunks;

	chunks = (new << shift) - (old << shift);

	return chunks >> shift;
}

static u64 adjust(struct mydomain *m, u64 rmid, u64 event, u64 chunks)
{
	struct mbm_event_state *s;

	switch (event) {
	case EV_LLC:
		return chunks;
	case EV_TOT:
		s = &m->rmids[rmid].state[0];
		break;
	case EV_LOC:
		s = &m->rmids[rmid].state[1];
		break;
	default: // TODO: TOT_RATE and LOC_RATE
		return 0;
	}

	if (!s->init) {
		s->chunks = 0;
		s->prev_msr = chunks;
		s->init = true;
		return 0;
	}

	return get_corrected_mbm_count(rmid, s->chunks + wrap(s->prev_msr, chunks));
}

struct rrmid_info {
	struct mydomain	*domain;
	u64		rmid;
	u64		event;
	u64		chunks;
};

static u64 snc_adjust_rmid(u64 rmid)
{
	if (arch_snc_nodes_per_l3_cache > 1) {
		int node = cpu_to_node(raw_smp_processor_id());

		rmid += (node % arch_snc_nodes_per_l3_cache) * num_rmids;
	}

	return rmid;
}

static void __rdt_rmid_read(void *info)
{
	struct rrmid_info *rr = info;
	struct rmid *cr, *r;
	struct mydomain *m;
	u64 chunks;

	m = rr->domain;

	if (rr->event <= EV_LOC) {
		wrmsrl(MSR_IA32_QM_EVTSEL, (snc_adjust_rmid(rr->rmid) << 32) | rr->event);
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
				wrmsrl(MSR_IA32_QM_EVTSEL, (snc_adjust_rmid(crmid) << 32) | rr->event);
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
	struct rrmid_info rr;
	struct mydomain *m;

	list_for_each_entry(m, &monitor.domains, list)
		if (m->id == domain_id)
			goto found;
	return ~0ull;
found:
	rr.domain = m;
	rr.rmid = rmid;
	rr.event = event;

	if (event <= EV_LOC)
		smp_call_function_any(&m->cpu_mask, __rdt_rmid_read, &rr, 1);
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
				s = &ri->mydomain->rmids[rmid].state[0];
			else
				s = &ri->mydomain->rmids[rmid].state[1];
			wrmsrl(MSR_IA32_QM_EVTSEL, (snc_adjust_rmid(rmid) << 32) | event);
			rdmsrl(MSR_IA32_QM_CTR, msr);
			now = jiffies;
			if (s->init) {
				addchunks = wrap(s->prev_msr, msr);
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

static void check_limbo(struct mydomain *m)
{
	struct rmid *r, *tmp;

	list_for_each_entry_safe(r, tmp, &limbo_rmids, list) {
		u64 rmid = r - rmid_array;
		u64 chunks;

		if (!(r->llc_busy_domains & BIT(m->id)))
			continue;
		wrmsrl(MSR_IA32_QM_EVTSEL, (snc_adjust_rmid(rmid) << 32) | EV_LLC);
		rdmsrl(MSR_IA32_QM_CTR, chunks);

		if (chunks <= llc_busy_threshold) {
			r->llc_busy_domains &= ~BIT(m->id);
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

		if (ri.eventmap)
			update_rmids(&ri);
		if (!list_empty(&limbo_rmids))
			check_limbo(m);

		if (!ri.eventmap && list_empty(&limbo_rmids)) {
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

	list_for_each_entry(m, &monitor.domains, list) {
		m->rmids[r - rmid_array].state[0].init = false;
		m->rmids[r - rmid_array].state[1].init = false;
	}

	return r - rmid_array;
}

void rmid_free(int rmid)
{
	struct rmid *r = &rmid_array[rmid];
	struct mydomain *m;

	if (!num_rmids)
		return;

	if (active_events[EV_LLC]) {
		list_for_each_entry(m, &monitor.domains, list)
			r->llc_busy_domains |= BIT(m->id);
		list_move_tail(&r->list, &limbo_rmids);
		init_rmid_polling();
	} else {
		list_move_tail(&r->list, &free_rmids);
	}

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

static void snc_remap_rmids(bool online)
{
	u64 val;

	if (arch_snc_nodes_per_l3_cache == 1)
		return;

	rdmsrl(MSR_RMID_SNC_CONFIG, val);
	if (online)
		val &= ~BIT_ULL(0);
	else
		val |= BIT_ULL(0);
	wrmsrl(MSR_RMID_SNC_CONFIG, val);
}

static void domain_update(struct resctrl_resource *r, int what, int cpu, void *domain)
{
	struct mydomain *m = domain;

	if (what == RESCTRL_DOMAIN_DELETE) {
		/* Last CPU in domain going offline, stop polling */
		m->kthread = NULL;
		snc_remap_rmids(false);
	} else if (what == RESCTRL_DOMAIN_DELETE_CPU && cpu == m->cpu) {
		/* Polling CPU for this domain going offline, pick another */
		m->kthread = NULL;
		m->cpu = -1;
		init_poll_one_domain(m);
	} else if (what == RESCTRL_DOMAIN_ADD) {
		/* New domain online, start polling */
		m->cpu = -1;
		snc_remap_rmids(true);
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
			m->rmids[0].state[0].init = false;
			m->rmids[0].state[1].init = false;
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

/* CPU models that support MSR_RMID_SNC_CONFIG */
static const struct x86_cpu_id snc_cpu_ids[] __initconst = {
	X86_MATCH_INTEL_FAM6_MODEL(ICELAKE_X, 0),
	X86_MATCH_INTEL_FAM6_MODEL(SAPPHIRERAPIDS_X, 0),
	X86_MATCH_INTEL_FAM6_MODEL(EMERALDRAPIDS_X, 0),
	X86_MATCH_INTEL_FAM6_MODEL(GRANITERAPIDS_X, 0),
	{}
};

/*
 * There isn't a simple h/w bit that indicates whether a CPU is running
 * in Sub NUMA Cluster (SNC) mode. Infer the state by comparing the
 * ratio of NUMA nodes to L3 cache instances.
 * It is not possible to accurately determine SNC state if the system is
 * booted with a maxcpus=N parameter. That distorts the ratio of SNC nodes
 * to L3 caches. It will be OK if system is booted with hyperthreading
 * disabled (since this doesn't affect the ratio).
 */
static __init int snc_get_config(void)
{
	unsigned long *node_caches;
	int mem_only_nodes = 0;
	int cpu, node, ret;
	int num_l3_caches;

	if (!x86_match_cpu(snc_cpu_ids))
		return 1;

	node_caches = bitmap_zalloc(nr_node_ids, GFP_KERNEL);
	if (!node_caches)
		return 1;

	cpus_read_lock();
	for_each_node(node) {
		cpu = cpumask_first(cpumask_of_node(node));
		if (cpu < nr_cpu_ids)
			set_bit(get_cpu_cacheinfo_id(cpu, 3), node_caches);
		else
			mem_only_nodes++;
	}
	cpus_read_unlock();

	num_l3_caches = bitmap_weight(node_caches, nr_node_ids);
	kfree(node_caches);

	if (!num_l3_caches)
		return 1;

	ret = (nr_node_ids - mem_only_nodes) / num_l3_caches;

	if (ret > 1)
		monitor.scope = RESCTRL_NODE;

	return ret;
}

static int __init rdt_monitor_init(void)
{
	u32 eax, ebx, ecx, edx;

	if (!boot_cpu_has(X86_FEATURE_CQM) || !boot_cpu_has(X86_FEATURE_CQM_LLC))
		return -ENODEV;

	arch_snc_nodes_per_l3_cache = snc_get_config();

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
	upscale = ebx / arch_snc_nodes_per_l3_cache;
	num_rmids = (ecx + 1) / arch_snc_nodes_per_l3_cache;
	rdt_mbm_apply_quirk(num_rmids);

	monitor.domain_size += num_rmids * sizeof(struct arch_rmid_state);

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
