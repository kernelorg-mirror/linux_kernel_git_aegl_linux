// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2023 Intel Corporation. */

#include "internal.h"

int resctrl_mkdir(struct kernfs_node *parent_kn, const char *name, umode_t mode)
{
	struct resctrl_node_info *prni;
	struct resctrl_group *rg, *prg;
	struct resctrl_node_info *rni;
	struct kernfs_node *kn;
	int ret = 0;

	if (IS_RESCTRL_REFCOUNT(parent_kn->priv))
		return -EPERM;

	if (strchr(name, '\n'))
		return -EINVAL;

	rni = kzalloc(sizeof(*rni) + sizeof(*rg), GFP_KERNEL);
	if (!rni)
		return -ENOMEM;
	rni->type = RESCTRL_GROUP;
	rg = (struct resctrl_group *)&rni->priv;

	prni = resctrl_kn_lock_live(parent_kn);
	if (!prni) {
		kfree(rni);
		ret = -ENOENT;
		goto unlock;
	}
	prg = (struct resctrl_group *)&prni->priv;

	switch (prni->type) {
	case RESCTRL_GROUP:
		if (!prg || prg->type != DIR_ROOT) {
			kfree(rni);
			ret = -EPERM;
			goto unlock;
		}
		rg->type = DIR_CTRL_MON;
		prni = parent_kn->priv;
		rg->parent = (struct resctrl_group *)&prni->priv;
		list_add(&rg->list, &all_ctrl_groups);
		INIT_LIST_HEAD(&rg->child_list);
		break;
	default:
		kfree(rni);
		ret = -EPERM;
		goto unlock;
	}

	kn = resctrl_add_dir(parent_kn, name, rni);
	if (!kn) {
		list_del(&rg->list);
		kfree(rni);
		ret = -EINVAL;
		goto unlock;
	}
	rni->kn = kn;
	kernfs_get(kn);

	kernfs_activate(kn);
unlock:
	resctrl_kn_unlock(parent_kn);

	return ret;
}
