// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2023 Intel Corporation. */

#include "internal.h"

static bool matchit(int dirtype, int file_flags)
{
	if ((dirtype == DIR_CTRL_MON || dirtype == DIR_ROOT) &&
	    (file_flags & RESCTRL_CTRLMON_FILE))
		return true;
	if (dirtype == DIR_MON && (file_flags & RESCTRL_MON_FILE))
		return true;

	return false;
}

static void resctrl_addctrlfiles_one(struct kernfs_node *kn, struct resctrl_resource *r,
				     struct resctrl_group *rg)
{
	struct resctrl_node_info *rni;
	struct resctrl_ctrlfileinfo *f;
	struct ctrl_file_info *tfi;
	umode_t mode;

	for (f = r->ctrlfiles; f->name; f++) {
		if (!matchit(rg->type, f->flags))
			continue;
		mode = (f->write) ? 0644 : 0444;
		rni = resctrl_add_file(kn, f->name, mode, RESCTRL_CTRLFILE);
		if (!rni)
			return;
		tfi = (struct ctrl_file_info *)&rni->priv;
		tfi->resctrl_ids = rg->resctrl_ids;
		tfi->show = f->show;
		tfi->write = f->write;
	}
	kernfs_activate(kn);
}

/* Add to all groups for a single resource */
void resctrl_addctrlfiles_all(struct resctrl_resource *r)
{
	struct resctrl_node_info *rni;
	struct resctrl_group *rg, *crg;

	list_for_each_entry(rg, &all_ctrl_groups, list) {
		if (rg->mode == RESCTRL_PSEUDO_LOCKED)
			continue;
		rni = (struct resctrl_node_info *)rg - 1;
		resctrl_addctrlfiles_one(rni->kn, r, rg);

		list_for_each_entry(crg, &rg->child_list, list) {
			rni = (struct resctrl_node_info *)crg - 1;
			resctrl_addctrlfiles_one(rni->kn, r, crg);
		}
	}
}

/* Add files to one directory for all resources */
void resctrl_addctrlfiles_dir(struct kernfs_node *kn, struct resctrl_group *rg)
{
	struct resctrl_resource *r;

	for_each_resource_by_cap(r, ctrlfiles)
		resctrl_addctrlfiles_one(kn, r, rg);
}

static void resctrl_delctrlfiles_one(struct kernfs_node *kn, struct resctrl_resource *r,
				     struct resctrl_group *rg, struct list_head *h)
{
	struct resctrl_ctrlfileinfo *f;

	for (f = r->ctrlfiles; f->name; f++) {
		if (!matchit(rg->type, f->flags))
			continue;
		resctrl_remove_file(f->name, kn, h);
	}
}

/* Delete files from all groups for one resource */
void resctrl_delctrlfiles_all(struct resctrl_resource *r, struct list_head *h)
{
	struct resctrl_node_info *rni;
	struct resctrl_group *rg, *crg;

	list_for_each_entry(rg, &all_ctrl_groups, list) {
		if (rg->mode == RESCTRL_PSEUDO_LOCKED)
			continue;
		rni = (struct resctrl_node_info *)rg - 1;
		resctrl_delctrlfiles_one(rni->kn, r, rg, h);

		list_for_each_entry(crg, &rg->child_list, list) {
			rni = (struct resctrl_node_info *)crg - 1;
			resctrl_delctrlfiles_one(rni->kn, r, crg, h);
		}
	}
}

/* Delete files from one directory for all resources */
void resctrl_delctrlfiles_dir(struct kernfs_node *kn, struct resctrl_group *rg,
			      struct list_head *h)
{
	struct resctrl_resource *r;

	for_each_resource_by_cap(r, ctrlfiles)
		resctrl_delctrlfiles_one(kn, r, rg, h);
	for_each_resource_by_cap(r, rmdir)
		if (rg != resctrl_default)
			r->rmdir(rg->resctrl_ids, rg->parent->resctrl_ids);
}
