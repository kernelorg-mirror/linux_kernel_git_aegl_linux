// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2023 Intel Corporation. */

#include "internal.h"

static void show_val(struct seq_file *m, struct resctrl_resource *r, struct resctrl_domain *d,
		     int ctrl_indx)
{
	int size = BITS_TO_LONGS(d->param);
	unsigned long *curval = d->ctrls;
	u32 cache_slice_size;
	int nbits;

	switch (r->schemata_fmt) {
	default:
	case RESCTRL_ULONG:
		seq_printf(m, "%lu", curval[ctrl_indx]);
		break;
	case RESCTRL_BITMASK:
		nbits = bitmap_weight(&curval[ctrl_indx * size], d->param);
		cache_slice_size = d->cache_size / d->param * nbits;
		if (r->scope == RESCTRL_L3CACHE)
			cache_slice_size /= arch_snc_nodes_per_l3_cache;
		seq_printf(m, "%u", cache_slice_size);
		break;
	}
}

static int size_seq_show(struct seq_file *m, struct resctrl_group *rg)
{
	struct resctrl_resource *r;
	struct resctrl_domain *d;
	char *sep;

	for_each_resource_by_cap(r, num_alloc_ids) {
		seq_printf(m, "%s: ", r->schemata_name);
		sep = "";
		list_for_each_entry(d, &r->domains, list) {
			seq_printf(m, "%s%d=", sep, d->id);
			show_val(m, r, d, arch_ctrl_id(rg->resctrl_ids));
			sep = ";";
		}
		seq_puts(m, "\n");
	}

	return 0;
}

bool resctrl_add_size_file(struct kernfs_node *parent_kn)
{
	struct resctrl_node_info *rni, *prni;
	struct core_file_info *cfi;

	rni = resctrl_add_file(parent_kn, "size", 0444, RESCTRL_COREFILE);
	if (!rni)
		return false;
	prni = parent_kn->priv;
	rni->flags = RESCTRL_LOCK_CPUS;
	cfi = (struct core_file_info *)&rni->priv;
	cfi->rg = (struct resctrl_group *)&prni->priv;
	cfi->show = size_seq_show;

	return true;
}

void resctrl_remove_size_file(struct kernfs_node *parent_kn, struct list_head *h)
{
	resctrl_remove_file("size", parent_kn, h);
}
