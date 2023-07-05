// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2023 Intel Corporation. */

#include "internal.h"

static ssize_t mode_write(char *buf, size_t nbytes, struct resctrl_group *rg,
			  struct kernfs_open_file *of)
{
	struct resctrl_resource *r;
	enum resctrl_mode mode;
	bool support = false;
	int ret = 0;

	if (nbytes == 0 || buf[nbytes - 1] != '\n')
		return -EINVAL;
	buf[nbytes - 1] = '\0';

	resctrl_last_cmd_clear();

	mode = rg->mode;

	if ((!strcmp(buf, "shareable") && mode == RESCTRL_SHARED) ||
	    (!strcmp(buf, "exclusive") && mode == RESCTRL_EXCLUSIVE))
		goto out;

	if (!strcmp(buf, "shareable")) {
		for_each_control_resource(r) {
			if (r->setmode)
				r->setmode(r, rg->resctrl_ids, RESCTRL_SHARED);
		}
		rg->mode = RESCTRL_SHARED;
	} else if (!strcmp(buf, "exclusive")) {
		for_each_control_resource(r) {
			if (r->setmode) {
				support = true;
				if (!r->setmode(r, rg->resctrl_ids, RESCTRL_TRY_EXCLUSIVE)) {
					resctrl_last_cmd_puts("Conflict\n");
					ret = -EINVAL;
					goto out;
				}
			}
		}
		if (!support) {
			ret = -EINVAL;
			goto out;
		}
		for_each_control_resource(r) {
			if (r->setmode)
				r->setmode(r, rg->resctrl_ids, RESCTRL_EXCLUSIVE);
		}
		rg->mode = RESCTRL_EXCLUSIVE;
	} else {
		resctrl_last_cmd_puts("Unknown or unsupported mode\n");
		ret = -EINVAL;
	}
out:
	return ret ?: nbytes;
}

static int mode_seq_show(struct seq_file *m, struct resctrl_group *rg)
{
	switch (rg->mode) {
	case RESCTRL_SHARED:
		seq_puts(m, "shareable\n");
		break;
	case RESCTRL_EXCLUSIVE:
		seq_puts(m, "exclusive\n");
		break;
	default:
		return -EINVAL;
	}

	return 0;
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
