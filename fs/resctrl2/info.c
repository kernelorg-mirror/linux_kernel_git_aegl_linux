// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2023 Intel Corporation. */

#include "internal.h"

static struct kernfs_node *kn_info;

static struct resctrl_group info_header = {
	.type = DIR_INFO
};

bool resctrl_add_info_dir(struct kernfs_node *parent_kn)
{
	kn_info = resctrl_add_dir(parent_kn, "info", &info_header);
	if (!kn_info)
		return false;

	return true;
}

void resctrl_addinfofiles(struct resctrl_resource *r)
{
	struct resctrl_node_info *rni;
	struct info_file_info *ifi;
	struct resctrl_fileinfo *f;
	struct kernfs_node *pkn;
	umode_t mode = 0;
	int *refcount;

	pkn = kernfs_find_and_get_ns(kn_info, r->infodir, NULL);
	if (!pkn)
		pkn = resctrl_add_dir(kn_info, r->infodir, NULL);
	if (!pkn)
		return;

	refcount = (int *)&pkn->priv;
	(*refcount)++;

	for (f = r->infofiles; f->name; f++) {
		if (f->show)
			mode |= 0444;
		rni = resctrl_add_file(pkn, f->name, mode, RESCTRL_INFOFILE);
		if (!rni)
			return;
		ifi = (struct info_file_info *)&rni->priv;
		ifi->show = f->show;
		(*refcount)++;
	}

	kernfs_activate(pkn);
}

void resctrl_delinfofiles(struct resctrl_resource *r, struct list_head *h)
{
	struct resctrl_fileinfo *f;
	struct kernfs_node *pkn;
	int *refcount;

	pkn = kernfs_find_and_get_ns(kn_info, r->infodir, NULL);
	if (!pkn)
		return;

	refcount = (int *)&pkn->priv;

	for (f = r->infofiles; f->name; f++) {
		resctrl_remove_file(f->name, pkn, h);
		(*refcount)--;
	}

	(*refcount)--;

	if (!*refcount)
		kernfs_remove(pkn);
}
