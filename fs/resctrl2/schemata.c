// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2023 Intel Corporation. */

#include "internal.h"

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

	return true;
}

void resctrl_remove_schemata_file(struct kernfs_node *parent_kn, struct list_head *h)
{
	resctrl_remove_file("schemata", parent_kn, h);
}
