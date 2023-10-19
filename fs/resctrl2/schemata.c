// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2023 Intel Corporation. */

#include "internal.h"

static void resetstaging(void)
{
	struct resctrl_resource *r;
	struct resctrl_domain *d;
	int ctrl_size = 1;

	for_each_resource_by_cap(r, num_alloc_ids) {
		list_for_each_entry(d, &r->domains, list) {
			unsigned long *curval, *staged;

			if (r->schemata_fmt == RESCTRL_BITMASK)
				ctrl_size = BITS_TO_LONGS(d->param);
			curval = d->ctrls;
			staged = curval + r->num_alloc_ids * ctrl_size;
			for (int i = 0; i < r->num_alloc_ids; i++)
				memcpy(&staged[i], &curval[i], ctrl_size * sizeof(unsigned long));
		}
	}
}

static bool parse(struct resctrl_resource *r, char *line, int ctrl_indx)
{
	struct resctrl_domain *d;
	char *dom = NULL, *id;
	unsigned long *staged;
	unsigned long dom_id;
	unsigned long ctrl;
	int ctrl_size;

next:
	if (!line || line[0] == '\0')
		return true;

	dom = strsep(&line, ";");
	id = strsep(&dom, "=");
	id = strim(id);
	if (!dom || kstrtoul(id, 10, &dom_id)) {
		resctrl_last_cmd_puts("Missing '=' or non-numeric domain\n");
		return false;
	}
	dom = strim(dom);
	list_for_each_entry(d, &r->domains, list) {
		if (d->id != dom_id)
			continue;

		switch (r->schemata_fmt) {
		case RESCTRL_BITMASK:
			ctrl_size = BITS_TO_LONGS(d->param);
			staged = d->ctrls + (r->num_alloc_ids + ctrl_indx) * ctrl_size;
			if (bitmap_parse(dom, UINT_MAX, staged, d->param)) {
				resctrl_last_cmd_printf("bad bitmap '%s'\n", dom);
				return false;
			}
			break;
		case RESCTRL_ULONG:
		default:
			if (kstrtoul(dom, 0, &ctrl)) {
				resctrl_last_cmd_printf("non-numeric char in '%s'\n", dom);
				return false;
			}
			if (ctrl > d->param) {
				resctrl_last_cmd_printf("parameter %lu too large (max=%lu)\n",
							ctrl, d->param);
				return false;
			}
			staged = d->ctrls + r->num_alloc_ids;
			staged[ctrl_indx] = ctrl;
			break;
		}
		goto next;
	}

	resctrl_last_cmd_printf("Domain %d not found\n", dom_id);

	return false;
}

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
	resetstaging();

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
		for_each_resource_by_cap(r, schemata_name) {
			if (strcmp(resname, r->schemata_name))
				continue;
			foundresource = true;
			break;
		}
		if (!foundresource) {
			resctrl_last_cmd_printf("Unknown resource '%s'\n", resname);
			ret = -EINVAL;
			goto out;
		}
		if (!parse(r, tok, arch_ctrl_id(rg->resctrl_ids))) {
			ret = -EINVAL;
			goto out;
		}
	}

	for_each_resource_by_cap(r, schemata_validate) {
		if (!r->schemata_validate(r)) {
			ret = -EINVAL;
			goto out;
		}
	}

	for_each_resource_by_cap(r, applychanges)
		r->applychanges(r);

out:
	resetstaging();

	return ret ?: nbytes;
}

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
	cfi->write = schemata_write;

	return true;
}

void resctrl_remove_schemata_file(struct kernfs_node *parent_kn, struct list_head *h)
{
	resctrl_remove_file("schemata", parent_kn, h);
}
