// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2023 Intel Corporation. */

#include "internal.h"

static int mode_seq_show(struct seq_file *m, struct resctrl_group *rg)
{
	switch (rg->mode) {
	case RESCTRL_SHARED:
		seq_puts(m, "shareable\n");
		return 0;
	case RESCTRL_EXCLUSIVE:
		seq_puts(m, "exclusive\n");
		return 0;
	default:
		break;
	}

	return -EINVAL;
}

bool resctrl_add_mode_file(struct kernfs_node *parent_kn)
{
	struct resctrl_node_info *rni, *prni;
	struct core_file_info *cfi;

	rni = resctrl_add_file(parent_kn, "mode", 0644, RESCTRL_COREFILE);
	if (!rni)
		return false;
	prni = parent_kn->priv;
	cfi = (struct core_file_info *)&rni->priv;
	cfi->rg = (struct resctrl_group *)&prni->priv;
	cfi->show = mode_seq_show;

	return true;
}

void resctrl_remove_mode_file(struct kernfs_node *parent_kn, struct list_head *h)
{
	resctrl_remove_file("mode", parent_kn, h);
}
