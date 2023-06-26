// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2023 Intel Corporation. */

#include "internal.h"

static ssize_t mode_write(struct kernfs_open_file *of, char *buf,
			  size_t nbytes, loff_t off)
{
	struct resctrl_resource *r;
	struct resctrl_group *rg;
	enum resctrl_mode mode;
	bool support = false;
	int ret = 0;

	if (nbytes == 0 || buf[nbytes - 1] != '\n')
		return -EINVAL;
	buf[nbytes - 1] = '\0';

	rg = resctrl_group_kn_lock_live(of->kn);
	if (!rg) {
		ret = -ENOENT;
		goto out;
	}

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
	resctrl_group_kn_unlock(of->kn);
	return ret ?: nbytes;
}

static int mode_seq_show(struct seq_file *m, void *arg)
{
	struct kernfs_open_file *of = m->private;
	struct resctrl_group *rg;
	int ret = 0;

	rg = resctrl_group_kn_lock_live(of->kn);
	if (rg) {
		switch (rg->mode) {
		case RESCTRL_SHARED:
			seq_puts(m, "shareable\n");
			break;
		case RESCTRL_EXCLUSIVE:
			seq_puts(m, "exclusive\n");
			break;
		default:
			ret = -EINVAL;
			break;
		}
	}
	resctrl_group_kn_unlock(of->kn);

	return ret;
}

static const struct kernfs_ops mode_ops = {
	.atomic_write_len	= PAGE_SIZE,
	.write			= mode_write,
	.seq_show		= mode_seq_show,
};

bool resctrl_add_mode_file(struct kernfs_node *parent_kn)
{
	struct kernfs_node *mode;

	mode = resctrl_add_file(parent_kn, "mode", 0644, &mode_ops, NULL);
	if (IS_ERR(mode))
		return false;

	return true;
}
