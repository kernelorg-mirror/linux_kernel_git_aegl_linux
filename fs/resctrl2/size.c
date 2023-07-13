// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2023 Intel Corporation. */

#include "internal.h"

static int size_seq_show(struct seq_file *m, struct resctrl_group *rg)
{
	struct resctrl_resource *r;

	for_each_control_resource(r) {
		if (r->size) {
			seq_printf(m, "%s: ", r->name);
			r->size(r, m, rg->resctrl_ids);
		}
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
