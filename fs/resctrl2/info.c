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
	struct kernfs_node *pkn;
	int *refcount;

	pkn = kernfs_find_and_get_ns(kn_info, r->infodir, NULL);
	if (!pkn)
		pkn = resctrl_add_dir(kn_info, r->infodir, NULL);
	if (!pkn)
		return;

	refcount = (int *)&pkn->priv;
	(*refcount)++;

	kernfs_activate(pkn);
}

void resctrl_delinfofiles(struct resctrl_resource *r)
{
	struct kernfs_node *pkn;
	int *refcount;

	pkn = kernfs_find_and_get_ns(kn_info, r->infodir, NULL);
	if (!pkn)
		return;

	refcount = (int *)&pkn->priv;
	(*refcount)--;

	if (!*refcount)
		kernfs_remove(pkn);
}
