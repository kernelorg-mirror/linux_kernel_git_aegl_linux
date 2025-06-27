// SPDX-License-Identifier: GPL-2.0-only
/*
 * Resource Director Technology(RDT)
 * - Intel Application Energy Telemetry
 *
 * Copyright (C) 2025 Intel Corporation
 *
 * Author:
 *    Tony Luck <tony.luck@intel.com>
 */

#define pr_fmt(fmt)   "resctrl: " fmt

#include <linux/cleanup.h>
#include <linux/cpu.h>
#include <linux/debugfs.h>
#include <linux/intel_vsec.h>
#include <linux/io.h>
#include <linux/minmax.h>
#include <linux/resctrl.h>
#include <linux/slab.h>

#include "internal.h"

/**
 * struct mmio_info - MMIO address information for one event group of a package.
 * @num_regions:	Number of telemetry regions on this package.
 * @addrs:		Array of MMIO addresses, one per telemetry region on this package.
 *
 * Provides convenient access to all MMIO addresses of one event group
 * for one package. Used when reading event data on a package.
 */
struct mmio_info {
	int		num_regions;
	void __iomem	*addrs[] __counted_by(num_regions);
};

/**
 * struct pmt_event - Telemetry event.
 * @id:		Resctrl event id.
 * @idx:	Counter index within each per-RMID block of counters.
 * @bin_bits:	Zero for integer valued events, else number bits in fixed-point.
 */
struct pmt_event {
	enum resctrl_event_id	id;
	int			idx;
	int			bin_bits;
};

#define EVT(_id, _idx, _bits) { .id = _id, .idx = _idx, .bin_bits = _bits }

/**
 * struct event_group - All information about a group of telemetry events.
 * @name:		Name for this group (used by boot rdt= option)
 * @pfg:		Points to the aggregated telemetry space information
 *			within the OOBMSM driver that contains data for all
 *			telemetry regions.
 * @pkginfo:		Per-package MMIO addresses of telemetry regions belonging to this group.
 * @guid:		Unique number per XML description file.
 * @num_rmids:		Number of RMIDS supported by this group. Adjusted downwards
 *			if enumeration from intel_pmt_get_regions_by_feature() indicates
 *			fewer RMIDs can be tracked simultaneously.
 * @mmio_size:		Number of bytes of MMIO registers for this group.
 * @num_events:		Number of events in this group.
 * @evts:		Array of event descriptors.
 */
struct event_group {
	/* Data fields for additional structures to manage this group. */
	char				*name;
	struct pmt_feature_group	*pfg;
	struct mmio_info		**pkginfo;

	/* Remaining fields initialized from XML file. */
	u32				guid;
	u32				num_rmids;
	size_t				mmio_size;
	int				num_events;
	struct pmt_event		evts[] __counted_by(num_events);
};

#define XML_MMIO_SIZE(num_rmids, num_events, num_extra_status)	\
	(((num_rmids) * (num_events) + (num_extra_status)) * sizeof(u64))

/*
 * Link: https://github.com/intel/Intel-PMT
 * File: xml/CWF/OOBMSM/RMID-ENERGY/cwf_aggregator.xml
 */
static struct event_group energy_0x26696143 = {
	.name		= "energy",
	.guid		= 0x26696143,
	.num_rmids	= 576,
	.mmio_size	= XML_MMIO_SIZE(576, 2, 3),
	.num_events	= 2,
	.evts				= {
		EVT(PMT_EVENT_ENERGY, 0, 18),
		EVT(PMT_EVENT_ACTIVITY, 1, 18),
	}
};

/*
 * Link: https://github.com/intel/Intel-PMT
 * File: xml/CWF/OOBMSM/RMID-PERF/cwf_aggregator.xml
 */
static struct event_group perf_0x26557651 = {
	.name		= "perf",
	.guid		= 0x26557651,
	.num_rmids	= 576,
	.mmio_size	= XML_MMIO_SIZE(576, 7, 3),
	.num_events	= 7,
	.evts				= {
		EVT(PMT_EVENT_STALLS_LLC_HIT, 0, 0),
		EVT(PMT_EVENT_C1_RES, 1, 0),
		EVT(PMT_EVENT_UNHALTED_CORE_CYCLES, 2, 0),
		EVT(PMT_EVENT_STALLS_LLC_MISS, 3, 0),
		EVT(PMT_EVENT_AUTO_C6_RES, 4, 0),
		EVT(PMT_EVENT_UNHALTED_REF_CYCLES, 5, 0),
		EVT(PMT_EVENT_UOPS_RETIRED, 6, 0),
	}
};

static struct event_group *known_event_groups[] = {
	&energy_0x26696143,
	&perf_0x26557651,
};

#define NUM_KNOWN_GROUPS ARRAY_SIZE(known_event_groups)

static bool skip_this_region(struct telemetry_region *tr, struct event_group *e)
{
	if (tr->guid != e->guid)
		return true;
	if (tr->plat_info.package_id >= topology_max_packages()) {
		pr_warn_once("Bad package %d in guid 0x%x\n", tr->plat_info.package_id,
			     tr->guid);
		return true;
	}
	if (tr->size < e->mmio_size) {
		pr_warn_once("MMIO space %zu too small for guid 0x%x\n", tr->size, e->guid);
		return true;
	}

	return false;
}

static void free_mmio_info(struct mmio_info **mmi)
{
	int num_pkgs = topology_max_packages();

	if (!mmi)
		return;

	for (int i = 0; i < num_pkgs; i++)
		kfree(mmi[i]);
	kfree(mmi);
}

DEFINE_FREE(mmio_info, struct mmio_info **, free_mmio_info(_T))

/*
 * Configure events from one pmt_feature_group.
 * 1) Count how many per package.
 * 2...) To be continued.
 */
static int configure_events(struct event_group *e, struct pmt_feature_group *p)
{
	struct mmio_info **pkginfo __free(mmio_info) = NULL;
	int *pkgcounts __free(kfree) = NULL;
	struct telemetry_region *tr;
	struct mmio_info *mmi;
	int num_pkgs;

	if (!rdt_is_software_feature_enabled(e->name))
		return -EINVAL;

	num_pkgs = topology_max_packages();

	/* Get per-package counts of telemetry_regions for this event group */
	for (int i = 0; i < p->count; i++) {
		tr = &p->regions[i];
		if (skip_this_region(tr, e))
			continue;

		if (e->pkginfo) {
			pr_warn_once("Duplicate telemetry information for guid 0x%x\n", e->guid);
			return -EINVAL;
		}

		/*
		 * Ignore event group with fewer RMIDs than can be loaded
		 * into the IA32_PQR_ASSOC MSR unless the user used
		 * the rdt= boot option to specifically ask for it to
		 * be enabled.
		 */
		if (tr->num_rmids < rdt_num_system_rmids &&
		    !rdt_is_software_feature_force_enabled(e->name))
			return -EINVAL;
		e->num_rmids = min(e->num_rmids, tr->num_rmids);

		if (!pkgcounts) {
			pkgcounts = kcalloc(num_pkgs, sizeof(*pkgcounts), GFP_KERNEL);
			if (!pkgcounts)
				return -ENOMEM;
		}
		pkgcounts[tr->plat_info.package_id]++;
	}

	if (!pkgcounts)
		return -ENODEV;

	/* Allocate array for per-package struct mmio_info data */
	pkginfo = kcalloc(num_pkgs, sizeof(*pkginfo), GFP_KERNEL);
	if (!pkginfo)
		return -ENOMEM;

	/*
	 * Allocate per-package mmio_info structures and initialize
	 * count of telemetry_regions in each one.
	 */
	for (int i = 0; i < num_pkgs; i++) {
		pkginfo[i] = kzalloc(struct_size(pkginfo[i], addrs, pkgcounts[i]), GFP_KERNEL);
		if (!pkginfo[i])
			return -ENOMEM;
		pkginfo[i]->num_regions = pkgcounts[i];
	}

	/* Save MMIO address(es) for each telemetry region in per-package structures */
	for (int i = 0; i < p->count; i++) {
		tr = &p->regions[i];
		if (skip_this_region(tr, e))
			continue;
		mmi = pkginfo[tr->plat_info.package_id];
		mmi->addrs[--pkgcounts[tr->plat_info.package_id]] = tr->addr;
	}
	e->pkginfo = no_free_ptr(pkginfo);

	for (int i = 0; i < e->num_events; i++) {
		enum resctrl_event_id eventid;

		eventid = e->evts[i].id;
		resctrl_enable_mon_event(eventid, true, e->evts[i].bin_bits, &e->evts[i]);
	}

	return 0;
}

DEFINE_FREE(intel_pmt_put_feature_group, struct pmt_feature_group *,
		if (!IS_ERR_OR_NULL(_T))
			intel_pmt_put_feature_group(_T))

/*
 * Make a request to the INTEL_PMT_DISCOVERY driver for the
 * pmt_feature_group for a specific feature. If there is
 * one the returned structure has an array of telemetry_region
 * structures. Each describes one telemetry aggregator.
 * Try to configure any with a known matching guid.
 */
static bool get_pmt_feature(enum pmt_feature_id feature)
{
	struct pmt_feature_group *p __free(intel_pmt_put_feature_group) = NULL;
	struct event_group **peg;
	bool ret;

	p = intel_pmt_get_regions_by_feature(feature);

	if (IS_ERR_OR_NULL(p))
		return false;

	for (peg = &known_event_groups[0]; peg < &known_event_groups[NUM_KNOWN_GROUPS]; peg++) {
		ret = configure_events(*peg, p);
		if (!ret) {
			(*peg)->pfg = no_free_ptr(p);
			return true;
		}
	}

	return false;
}

static ssize_t status_read(struct file *f, char __user *buf, size_t count, loff_t *off)
{
	void __iomem *info = (void __iomem *)f->f_inode->i_private;
	char status[32];
	int len;

	len = sprintf(status, "%llu\n", readq(info));

	return simple_read_from_buffer(buf, count, off, status, len);
}

static const struct file_operations status_fops = {
	.read = status_read
};

static void make_status_files(struct dentry *dir, struct event_group *e, int pkg, int instance)
{
	void *info = (void __force *)e->pkginfo[pkg]->addrs[instance] + e->mmio_size;
	char name[64];

	sprintf(name, "%s_pkg%d_agg%d_data_loss_count", e->name, pkg, instance);
	debugfs_create_file(name, 0400, dir, info - 24, &status_fops);

	sprintf(name, "%s_pkg%d_agg%d_data_loss_timestamp", e->name, pkg, instance);
	debugfs_create_file(name, 0400, dir, info - 16, &status_fops);

	sprintf(name, "%s_pkg%d_agg%d_last_update_timestamp", e->name, pkg, instance);
	debugfs_create_file(name, 0400, dir, info - 8, &status_fops);
}

static void create_debug_event_status_files(struct dentry *dir, struct event_group *e)
{
	int num_pkgs = topology_max_packages();

	for (int i = 0; i < num_pkgs; i++)
		for (int j = 0; j < e->pkginfo[i]->num_regions; j++)
			make_status_files(dir, e, i, j);
}

static void create_debugfs_status_file(struct rdt_resource *r)
{
	struct event_group **eg;
	struct dentry *infodir;

	infodir = resctrl_debugfs_mon_info_arch_mkdir(r);
	for (eg = &known_event_groups[0]; eg < &known_event_groups[NUM_KNOWN_GROUPS]; eg++) {
		if (!(*eg)->pfg)
			continue;
		create_debug_event_status_files(infodir, *eg);
	}
}

/*
 * Ask OOBMSM discovery driver for all the RMID based telemetry groups
 * that it supports.
 */
bool intel_aet_get_events(void)
{
	struct rdt_resource *r = &rdt_resources_all[RDT_RESOURCE_PERF_PKG].r_resctrl;
	struct event_group **eg;
	bool ret1, ret2;

	ret1 = get_pmt_feature(FEATURE_PER_RMID_ENERGY_TELEM);
	ret2 = get_pmt_feature(FEATURE_PER_RMID_PERF_TELEM);

	for (eg = &known_event_groups[0]; eg < &known_event_groups[NUM_KNOWN_GROUPS]; eg++) {
		if (!(*eg)->pfg)
			continue;
		if (r->num_rmid)
			r->num_rmid = min(r->num_rmid, (*eg)->num_rmids);
		else
			r->num_rmid = (*eg)->num_rmids;
		pr_info("%s %s monitoring detected\n", r->name, (*eg)->name);

		r->mon_capable = true;
	}

	if (ret1 || ret2)
		create_debugfs_status_file(r);

	return ret1 || ret2;
}

void __exit intel_aet_exit(void)
{
	struct event_group **peg;

	for (peg = &known_event_groups[0]; peg < &known_event_groups[NUM_KNOWN_GROUPS]; peg++) {
		if ((*peg)->pfg) {
			intel_pmt_put_feature_group((*peg)->pfg);
			(*peg)->pfg = NULL;
		}
		free_mmio_info((*peg)->pkginfo);
	}
}

#define DATA_VALID	BIT_ULL(63)
#define DATA_BITS	GENMASK_ULL(62, 0)

/*
 * Read counter for an event on a domain (summing all aggregators
 * on the domain).
 */
int intel_aet_read_event(int domid, int rmid, enum resctrl_event_id eventid,
			 void *arch_priv, u64 *val)
{
	struct pmt_event *pevt = arch_priv;
	struct mmio_info *mmi;
	struct event_group *e;
	u64 evtcount;
	void *pevt0;
	int idx;

	pevt0 = pevt - pevt->idx;
	e = container_of(pevt0, struct event_group, evts);
	idx = rmid * e->num_events;
	idx += pevt->idx;
	mmi = e->pkginfo[domid];

	if (idx * sizeof(u64) + sizeof(u64) > e->mmio_size) {
		pr_warn_once("MMIO index %d out of range\n", idx);
		return -EIO;
	}

	for (int i = 0; i < mmi->num_regions; i++) {
		evtcount = readq(mmi->addrs[i] + idx * sizeof(u64));
		if (!(evtcount & DATA_VALID))
			return -EINVAL;
		*val += evtcount & DATA_BITS;
	}

	return 0;
}
