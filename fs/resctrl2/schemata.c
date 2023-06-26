// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2023 Intel Corporation. */

#include "internal.h"

static ssize_t schemata_write(struct kernfs_open_file *of, char *buf,
			      size_t nbytes, loff_t off)
{
	struct resctrl_resource *r;
	struct resctrl_group *rg;
	char *tok, *resname;
	bool foundresource;
	int ret = 0;

	/* Valid input requires a trailing newline */
	if (nbytes == 0 || buf[nbytes - 1] != '\n')
		return -EINVAL;
	buf[nbytes - 1] = '\0';

	cpus_read_lock();
	rg = resctrl_group_kn_lock_live(of->kn);
	if (!rg) {
		ret = -ENOENT;
		goto out;
	}

	resctrl_last_cmd_clear();

	for_each_control_resource(r)
		r->resetstaging(r, rg->resctrl_ids);

	while ((tok = strsep(&buf, "\n")) != NULL) {
		resname = strim(strsep(&tok, ":"));
		if (!tok) {
			resctrl_last_cmd_puts("Missing ':'\n");
			ret = -EINVAL;
			goto out;
		}
		if (tok[0] == '\0') {
			resctrl_last_cmd_printf("Missing '%s' value\n", resname);
			ret = -EINVAL;
			goto out;
		}
		foundresource = false;
		for_each_control_resource(r) {
			if (!strcmp(resname, r->name)) {
				ret = r->parse(r, tok, rg->resctrl_ids);
				if (ret < 0)
					goto out;
				foundresource = true;
				break;
			}
		}
		if (!foundresource) {
			resctrl_last_cmd_printf("Unknown resource '%s'\n", resname);
			ret = -EINVAL;
			goto out;
		}
	}

	for_each_control_resource(r)
		r->applychanges(r, rg->resctrl_ids);
out:
	for_each_control_resource(r)
		r->resetstaging(r, rg->resctrl_ids);

	resctrl_group_kn_unlock(of->kn);
	cpus_read_unlock();
	return ret ?: nbytes;
}

static int schemata_seq_show(struct seq_file *m, void *arg)
{
	struct kernfs_open_file *of = m->private;
	struct resctrl_resource *r;
	struct resctrl_group *rg;
	int ret = 0;

	rg = resctrl_group_kn_lock_live(of->kn);
	if (!rg) {
		ret = -ENOENT;
		goto out;
	}

	for_each_control_resource(r) {
		seq_printf(m, "%s: ", r->name);
		r->show(r, m, rg->resctrl_ids);
	}

out:
	resctrl_group_kn_unlock(of->kn);
	return ret;
}

static const struct kernfs_ops schemata_ops = {
	.atomic_write_len	= PAGE_SIZE,
	.write			= schemata_write,
	.seq_show		= schemata_seq_show,
};

bool resctrl_add_schemata_file(struct kernfs_node *parent_kn)
{
	struct kernfs_node *schemata;

	schemata = resctrl_add_file(parent_kn, "schemata", 0644, &schemata_ops, NULL);
	if (IS_ERR(schemata))
		return false;

	return true;
}
