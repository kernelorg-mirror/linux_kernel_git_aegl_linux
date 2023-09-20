// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2023 Intel Corporation. */

#include "internal.h"

static void show_val(struct seq_file *m, struct resctrl_resource *r, struct resctrl_domain *d,
		     int ctrl_indx)
{
	unsigned long *curval = d->ctrls;
	int size;

	switch (r->schemata_fmt) {
	default:
	case RESCTRL_ULONG:
		seq_printf(m, "%lu", curval[ctrl_indx]);
		break;
	case RESCTRL_BITMASK:
		size = BITS_TO_LONGS(d->param);

		seq_printf(m, "%*pb", d->param, &curval[ctrl_indx * size]);
	}
}

static int schemata_seq_show(struct seq_file *m, struct resctrl_group *rg)
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

bool resctrl_add_schemata_file(struct kernfs_node *parent_kn)
{
	struct resctrl_node_info *rni, *prni;
	struct core_file_info *cfi;

	rni = resctrl_add_file(parent_kn, "schemata", 0644, RESCTRL_COREFILE);
	if (!rni)
		return false;
	prni = parent_kn->priv;
	rni->flags = RESCTRL_LOCK_CPUS;
	cfi = (struct core_file_info *)&rni->priv;
	cfi->rg = (struct resctrl_group *)&prni->priv;
	cfi->show = schemata_seq_show;

	return true;
}

void resctrl_remove_schemata_file(struct kernfs_node *parent_kn, struct list_head *h)
{
	resctrl_remove_file("schemata", parent_kn, h);
}
