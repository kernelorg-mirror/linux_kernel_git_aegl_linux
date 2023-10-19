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

bool resctrl_overlap_in_domain(struct resctrl_resource *r, struct resctrl_domain *d,
			       int ctrl_indx, bool want_excl, bool check_staged)
{
	unsigned long *my_mask, *overlap;
	struct resctrl_group *rrg;
	int size;

	size = BITS_TO_LONGS(d->param);
	if (check_staged)
		my_mask = &d->ctrls[(r->num_alloc_ids + ctrl_indx) * size];
	else
		my_mask = &d->ctrls[ctrl_indx * size];
	overlap = bitmap_alloc(d->param, GFP_KERNEL);
	if (!overlap)
		return true;

	if (want_excl && d->share_bits) {
		bitmap_and(overlap, my_mask, d->share_bits, d->param);
		if (!bitmap_empty(overlap, d->param)) {
			kfree(overlap);
			return true;
		}
	}

	list_for_each_entry(rrg, &all_ctrl_groups, list) {
		int other_ctrl_indx = arch_ctrl_id(rrg->resctrl_ids);

		if (ctrl_indx == other_ctrl_indx)
			continue;
		if (!want_excl && rrg->mode == RESCTRL_SHARED)
			continue;
		bitmap_and(overlap, my_mask, &d->ctrls[other_ctrl_indx * size], d->param);
		if (!bitmap_empty(overlap, d->param)) {
			kfree(overlap);
			return true;
		}
	}
	kfree(overlap);

	return false;
}

static bool choose_bitmask(struct resctrl_resource *r, struct resctrl_group *rg, struct resctrl_domain *d)
{
	struct resctrl_group *rrg;
	unsigned long *new_mask;
	int ctrl_indx, indx;
	bool ret = false;
	int size;

	size = BITS_TO_LONGS(d->param);
	new_mask = bitmap_alloc(d->param, GFP_KERNEL);
	if (!new_mask)
		return ret;

	bitmap_fill(new_mask, d->param);

	ctrl_indx = arch_ctrl_id(rg->resctrl_ids);
	list_for_each_entry(rrg, &all_ctrl_groups, list) {
		if (rrg->mode != RESCTRL_EXCLUSIVE)
			continue;
		indx = arch_ctrl_id(rrg->resctrl_ids);
		bitmap_andnot(new_mask, new_mask, &d->ctrls[indx * size], d->param);
		ret = true;
	}
	if (ret)
		bitmap_copy(&d->ctrls[(r->num_alloc_ids + ctrl_indx) * size], new_mask, d->param);

	return ret;
}

void resctrl_fixup_exclusive(struct resctrl_group *rg)
{
	struct resctrl_resource *r;
	struct resctrl_domain *d;
	bool update;

	for_each_resource_by_cap(r, num_alloc_ids) {
		if (r->schemata_fmt != RESCTRL_BITMASK)
			continue;
		update = false;
		list_for_each_entry(d, &r->domains, list)
			if (choose_bitmask(r, rg, d))
				update = true;

		if (update)
			r->applychanges(r);
	}
}

static ssize_t mode_write(char *buf, size_t nbytes, struct resctrl_group *rg, struct kernfs_open_file *of)
{
	enum resctrl_mode new_mode;
	struct resctrl_resource *r;
	struct resctrl_domain *d;

	if (nbytes == 0 || buf[nbytes - 1] != '\n')
		return -EINVAL;
	buf[nbytes - 1] = '\0';

	resctrl_last_cmd_clear();

	if (!strcmp(buf, "shareable")) {
		new_mode = RESCTRL_SHARED;
	} else if (!strcmp(buf, "exclusive")) {
		new_mode = RESCTRL_EXCLUSIVE;
	} else {
		resctrl_last_cmd_puts("Unknown or unsupported mode\n");
		return -EINVAL;
	}

	if (rg->mode == new_mode)
		return nbytes;
	if (new_mode == RESCTRL_SHARED) {
		rg->mode = new_mode;
		return nbytes;
	}

	for_each_resource_by_cap(r, num_alloc_ids) {
		if (r->schemata_fmt != RESCTRL_BITMASK)
			continue;
		list_for_each_entry(d, &r->domains, list) {
			if (resctrl_overlap_in_domain(r, d, arch_ctrl_id(rg->resctrl_ids), true, false)) {
				resctrl_last_cmd_printf("overlap with resource %s\n", r->schemata_name);
				return -EINVAL;
			}
		}
	}

	rg->mode = new_mode;

	return nbytes;
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
