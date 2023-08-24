// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2023 Intel Corporation. */

#include "internal.h"

static struct mode_names {
	char			*name;
	enum resctrl_mode	mode;
} mode_names[] = {
	{ .name = "shareable",		.mode = RESCTRL_SHARED },
	{ .name = "exclusive",		.mode = RESCTRL_EXCLUSIVE },
	{ .name = "pseudo-locksetup",	.mode = RESCTRL_PSEUDO_LOCK_SETUP },
	{ .name = "pseudo-locked",	.mode = RESCTRL_PSEUDO_LOCKED },
	{ }
};

static int is_pseudo_lock_ok(struct resctrl_group *rg)
{
	if (!resctrl_pseudolock_available) {
		resctrl_last_cmd_puts("Pseudo-lock not supported\n");
		return -ENODEV;
	}
	if (rg == resctrl_default) {
		resctrl_last_cmd_puts("Cannot pseudo-lock default group\n");
		return -EINVAL;
	}
	if (resctrl_tasks_assigned(rg)) {
		resctrl_last_cmd_puts("Tasks assigned to resource group\n");
		return -EINVAL;
	}
	if (!list_empty(&rg->child_list)) {
		resctrl_last_cmd_puts("Monitoring in progress\n");
		return -EINVAL;
	}
	if (!cpumask_empty(&rg->cpu_mask)) {
		resctrl_last_cmd_puts("CPUs assigned to resource group\n");
		return -EINVAL;
	}

	return 0;
}

static ssize_t mode_write(char *buf, size_t nbytes, struct resctrl_group *rg,
			  struct kernfs_open_file *of, struct list_head *h)
{
	enum resctrl_mode mode, newmode;
	struct resctrl_resource *r;
	struct mode_names *mn;
	bool support = false;
	int ret = 0;

	if (nbytes == 0 || buf[nbytes - 1] != '\n')
		return -EINVAL;
	buf[nbytes - 1] = '\0';

	resctrl_last_cmd_clear();

	for (mn = mode_names; mn->name; mn++)
		if (!strcmp(buf, mn->name))
			goto found;
	resctrl_last_cmd_puts("Unknown or unsupported mode\n");
	return -EINVAL;
found:
	newmode = mn->mode;
	mode = rg->mode;

	if (mode == newmode)
		goto out;
	if (mode == RESCTRL_PSEUDO_LOCKED) {
		resctrl_last_cmd_puts("Cannot change pseudo-locked group\n");
		ret = -EINVAL;
		goto out;
	}

	if (newmode == RESCTRL_SHARED) {
		for_each_resource_by_cap(r, setmode)
			r->setmode(r, rg->resctrl_ids, RESCTRL_SHARED);
		rg->mode = RESCTRL_SHARED;
	} else if (newmode == RESCTRL_EXCLUSIVE) {
		for_each_resource_by_cap(r, setmode) {
			support = true;
			if (!r->setmode(r, rg->resctrl_ids, RESCTRL_TRY_EXCLUSIVE)) {
				resctrl_last_cmd_puts("Conflict\n");
				ret = -EINVAL;
				goto out;
			}
		}
		if (!support) {
			ret = -EINVAL;
			goto out;
		}
		for_each_resource_by_cap(r, setmode)
			r->setmode(r, rg->resctrl_ids, RESCTRL_EXCLUSIVE);
		rg->mode = RESCTRL_EXCLUSIVE;
	} else if (newmode == RESCTRL_PSEUDO_LOCK_SETUP) {
		ret = is_pseudo_lock_ok(rg);
		if (ret)
			goto out;
		for_each_resource_by_cap(r, setmode) {
			if (r->setmode(r, rg->resctrl_ids, RESCTRL_PSEUDO_LOCK_SETUP)) {
				rg->mode = newmode;
				goto out;
			}
		}
		resctrl_last_cmd_puts("Conflict\n");
		ret = -EINVAL;
	} else {
		resctrl_last_cmd_puts("Unknown or unsupported mode\n");
		ret = -EINVAL;
	}
out:
	return ret ?: nbytes;
}

static int mode_seq_show(struct seq_file *m, struct resctrl_group *rg)
{
	struct mode_names *mn;

	for (mn = mode_names; mn->name; mn++) {
		if (mn->mode == rg->mode) {
			seq_printf(m, "%s\n", mn->name);
			return 0;
		}
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
	cfi->write = mode_write;

	return true;
}

void resctrl_remove_mode_file(struct kernfs_node *parent_kn, struct list_head *h)
{
	resctrl_remove_file("mode", parent_kn, h);
}
