// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2023 Intel Corporation. */

#include "internal.h"

bool resctrl_populate_dir(struct kernfs_node *parent_kn, struct resctrl_group *rg)
{
	if (!resctrl_add_task_file(parent_kn))
		return false;

	if ((rg->type == DIR_ROOT || rg->type == DIR_CTRL_MON)) {
		if (!resctrl_add_schemata_file(parent_kn))
			return false;
	}

	return true;
}

static void resctrl_depopulate_dir(struct kernfs_node *parent_kn, struct resctrl_group *rg,
				   struct list_head *h)
{
	resctrl_remove_task_file(parent_kn, h);

	if ((rg->type == DIR_ROOT || rg->type == DIR_CTRL_MON))
		resctrl_remove_schemata_file(parent_kn, h);
}

static void resctrl_group_remove(struct resctrl_node_info *rni)
{
	kernfs_put(rni->kn);
	kfree(rni);
}

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
		if (!arch_alloc_resctrl_ids(rg)) {
			kfree(rni);
			ret = -ENOSPC;
			goto unlock;
		}
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

	resctrl_populate_dir(kn, rg);

	kernfs_activate(kn);
unlock:
	resctrl_kn_unlock(parent_kn);

	return ret;
}

static void resctrl_rmdir_ctrl(struct resctrl_group *rg, struct list_head *h)
{
	struct resctrl_node_info *rni;

	/* Give any tasks back to the default group */
	resctrl_move_group_tasks(rg, rg->parent);

	rni = (struct resctrl_node_info *)rg - 1;

	resctrl_depopulate_dir(rni->kn, rg, h);
	arch_free_resctrl_ids(rg);
	list_del(&rg->list);

	rni->flags |= RESCTRL_DELETED;
	kernfs_remove(rni->kn);
}

int resctrl_rmdir(struct kernfs_node *kn)
{
	struct resctrl_node_info *rni;
	struct resctrl_group *rg;
	LIST_HEAD(clean_list);
	int ret = 0;

	if (IS_RESCTRL_REFCOUNT(kn->priv))
		return -EPERM;

	rni = resctrl_kn_lock_live(kn);
	rg = (struct resctrl_group *)&rni->priv;
	if (!rni || rg->type != DIR_CTRL_MON) {
		ret = -EPERM;
		goto out;
	}

	if (rg->type == DIR_CTRL_MON)
		resctrl_rmdir_ctrl(rg, &clean_list);

out:
	resctrl_kn_unlock(kn);
	resctrl_node_file_cleanup(&clean_list);

	return ret;
}

void resctrl_rmdir_all_sub(bool is_umount, struct list_head *h)
{
	struct resctrl_group *rg, *tmp;
	struct resctrl_node_info *rni;

	list_for_each_entry_safe(rg, tmp, &all_ctrl_groups, list) {
		rni = (struct resctrl_node_info *)rg - 1;

		if (is_umount)
			resctrl_depopulate_dir(rni->kn, rg, h);

		/* Remove each group other than root */
		if (rg->type == DIR_ROOT)
			continue;

		arch_free_resctrl_ids(rg);

		kernfs_remove(rni->kn);
		list_add(&rni->clean_list, h);
		list_del(&rg->list);

		if (atomic_read(&rni->waitcount) != 0)
			rni->flags |= RESCTRL_DELETED;
		else
			resctrl_group_remove(rni);
	}
}
