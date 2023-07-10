// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2023 Intel Corporation. */

#include "internal.h"

static ssize_t schemata_write(char *buf, size_t nbytes, struct resctrl_group *rg,
			      struct kernfs_open_file *of)
{
	struct resctrl_resource *r;
	char *tok, *resname;
	bool foundresource;
	int ret = 0;

	/* Valid input requires a trailing newline */
	if (nbytes == 0 || buf[nbytes - 1] != '\n')
		return -EINVAL;
	buf[nbytes - 1] = '\0';

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

	return ret ?: nbytes;
}

static int schemata_seq_show(struct seq_file *m, struct resctrl_group *rg)
{
	struct resctrl_resource *r;

	for_each_control_resource(r) {
		seq_printf(m, "%s: ", r->name);
		r->show(r, m, rg->resctrl_ids);
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
	cfi->write = schemata_write;

	return true;
}

void resctrl_remove_schemata_file(struct kernfs_node *parent_kn, struct list_head *h)
{
	resctrl_remove_file("schemata", parent_kn, h);
}
