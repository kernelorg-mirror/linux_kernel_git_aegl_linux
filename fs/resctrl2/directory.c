// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2023 Intel Corporation. */

#include "internal.h"

static struct resctrl_node_info mongroup_header = {
	.type = RESCTRL_MONGROUP
};

static struct resctrl_node_info mondata_header = {
	.type = RESCTRL_MONDATA
};

bool resctrl_populate_dir(struct kernfs_node *parent_kn, struct resctrl_group *rg)
{
	struct resctrl_resource *r;
	struct kernfs_node *kn;

	if (!resctrl_add_task_file(parent_kn))
		return false;

	if ((rg->type == DIR_ROOT || rg->type == DIR_CTRL_MON)) {
		if (!resctrl_add_schemata_file(parent_kn))
			return false;
		if (!resctrl_add_mode_file(parent_kn))
			return false;
	}

	if (!resctrl_add_cpus_file(parent_kn))
		return false;

	if (!resctrl_add_dir(parent_kn, "mon_groups", &mongroup_header))
		return false;

	kn = resctrl_add_dir(parent_kn, "mon_data", &mondata_header);
	if (!kn)
		return false;
	rg->mondata = kn;

	for_each_monitor_resource(r)
		if (r->mon_domain_dir)
			resctrl_create_domain_files(rg->mondata, r, rg);

	return true;
}

static void resctrl_depopulate_dir(struct kernfs_node *parent_kn, struct resctrl_group *rg, struct list_head *h)
{
	resctrl_remove_task_file(parent_kn, h);
	resctrl_remove_cpus_file(parent_kn, h);
	if ((rg->type == DIR_ROOT || rg->type == DIR_CTRL_MON)) {
		resctrl_remove_schemata_file(parent_kn, h);
		resctrl_remove_mode_file(parent_kn, h);
	}
}

void resctrl_group_remove(struct resctrl_node_info *rni)
{
	kernfs_put(rni->kn);
	kfree(rni);
}

int resctrl_mkdir(struct kernfs_node *parent_kn, const char *name, umode_t mode)
{
	struct resctrl_group *rg;
	struct resctrl_node_info *prni;
	struct resctrl_resource *r;
	struct resctrl_node_info *rni;
	struct kernfs_node *kn;
	int ret = 0;

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

	switch (prni->type) {
	case RESCTRL_GROUP:
		rg->type = DIR_CTRL_MON;
		prni = parent_kn->priv;
		rg->parent = (struct resctrl_group *)&prni->priv;
		rg->mode = RESCTRL_SHARED;
		if (!arch_alloc_resctrl_ids(rg)) {
			kfree(rni);
			ret = -ENOSPC;
			goto unlock;
		}

		for_each_control_resource(r)
			if (r->setmode)
				r->setmode(r, rg->resctrl_ids, RESCTRL_SHARED);

		list_add(&rg->list, &all_ctrl_groups);
		INIT_LIST_HEAD(&rg->child_list);
		break;
	case RESCTRL_MONGROUP:
		rg->type = DIR_MON;
		prni = parent_kn->parent->priv;
		rg->parent = (struct resctrl_group *)prni->priv;
		if (!arch_alloc_resctrl_ids(rg)) {
			kfree(rg);
			ret = -ENOSPC;
			goto unlock;
		}
		list_add(&rg->list, &rg->parent->child_list);
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

static void free_all_child_resctrlgrp(struct resctrl_group *rg, struct list_head *h)
{
	struct resctrl_group *sentry, *stmp;
	struct resctrl_node_info *rni;
	struct resctrl_resource *r;
	struct list_head *head;

	for_each_monitor_resource(r)
		if (r->mon_domain_dir)
			resctrl_remove_domain_files(rg->mondata, r, h);

	head = &rg->child_list;
	list_for_each_entry_safe(sentry, stmp, head, list) {
		rni = (struct resctrl_node_info *)sentry - 1;
		arch_free_resctrl_ids(sentry);

		for_each_monitor_resource(r)
			if (r->mon_domain_dir)
				resctrl_remove_domain_files(sentry->mondata, r, h);

		list_del(&sentry->list);

		if (rg->type == DIR_ROOT)
			kernfs_remove(rni->kn);

		if (atomic_read(&rni->waitcount) != 0)
			rni->flags |= RESCTRL_DELETED;
		else
			resctrl_group_remove(rni);
	}
}

static void resctrl_rmdir_ctrl(struct resctrl_group *rg, struct cpumask *mask, struct list_head *h)
{
	struct resctrl_node_info *rni;
	struct resctrl_resource *r;
	int cpu;

	/* Give any tasks back to the default group */
	resctrl_move_group_tasks(rg, rg->parent, mask);

	/* Give any CPUs back to the default group */
	cpumask_or(&resctrl_default->cpu_mask,
		   &resctrl_default->cpu_mask, &rg->cpu_mask);

	/* Update resctrl_ids of the moved CPUs first */
	for_each_cpu(cpu, &rg->cpu_mask)
		per_cpu(resctrl_per_cpu_state.default_resctrl_ids, cpu) = arch_resctrl_default_ids;

	/*
	 * Update the MSR on moved CPUs and CPUs which have moved
	 * task running on them.
	 */
	cpumask_or(mask, mask, &rg->cpu_mask);
	update_resctrl_ids(mask, NULL);

	rni = (struct resctrl_node_info *)rg - 1;
	resctrl_depopulate_dir(rni->kn, rg, h);

	/*
	 * Free all the child monitor groups.
	 */
	free_all_child_resctrlgrp(rg, h);

	for_each_control_resource(r)
		if (r->setmode)
			r->setmode(r, rg->resctrl_ids, RESCTRL_FREE);

	arch_free_resctrl_ids(rg);
	list_del(&rg->list);

	rni->flags |= RESCTRL_DELETED;
	kernfs_remove(rni->kn);
}

static void resctrl_rmdir_mon(struct resctrl_group *rg, struct cpumask *mask, struct list_head *h)
{
	struct resctrl_group *prg = rg->parent;
	struct resctrl_node_info *rni;
	struct resctrl_resource *r;
	int cpu;

	/* Give any tasks back to the parent group */
	resctrl_move_group_tasks(rg, prg, mask);

	/* Update per cpu resctrl_ids of the moved CPUs first */
	for_each_cpu(cpu, &rg->cpu_mask)
		per_cpu(resctrl_per_cpu_state.default_resctrl_ids, cpu) = prg->resctrl_ids;
	/*
	 * Update the MSR on moved CPUs and CPUs which have moved
	 * task running on them.
	 */
	cpumask_or(mask, mask, &rg->cpu_mask);
	update_resctrl_ids(mask, NULL);

	rni = (struct resctrl_node_info *)rg - 1;
	resctrl_depopulate_dir(rni->kn, rg, h);

	for_each_monitor_resource(r)
		if (r->mon_domain_dir)
			resctrl_remove_domain_files(rg->mondata, r, h);

	rni->flags |= RESCTRL_DELETED;
	arch_free_resctrl_ids(rg);

	/*
	 * Remove the group from parent's list of children
	 */
	WARN_ON(list_empty(&prg->child_list));
	list_del(&rg->list);

	kernfs_remove(rni->kn);
}

int resctrl_rmdir(struct kernfs_node *kn)
{
	struct resctrl_node_info *rni;
	LIST_HEAD(mon_file_clean_list);
	struct resctrl_group *rg;
	cpumask_var_t tmpmask;
	int ret = 0;

	if (!zalloc_cpumask_var(&tmpmask, GFP_KERNEL))
		return -ENOMEM;
	rni = resctrl_kn_lock_live(kn);
	rg = (struct resctrl_group *)&rni->priv;
	if (!rni || (rg->type != DIR_CTRL_MON && rg->type != DIR_MON)) {
		ret = -EPERM;
		goto out;
	}

	if (rg->type == DIR_CTRL_MON)
		resctrl_rmdir_ctrl(rg, tmpmask, &mon_file_clean_list);
	else
		resctrl_rmdir_mon(rg, tmpmask, &mon_file_clean_list);

out:
	resctrl_kn_unlock(kn);
	resctrl_node_file_cleanup(&mon_file_clean_list);
	free_cpumask_var(tmpmask);

	return ret;
}

void resctrl_rmdir_all_sub(struct list_head *h)
{
	struct resctrl_group *rg, *tmp;
	struct resctrl_node_info *rni;

	list_for_each_entry_safe(rg, tmp, &all_ctrl_groups, list) {
		/* Free any child resource ids */
		free_all_child_resctrlgrp(rg, h);

		/* Remove each group other than root */
		if (rg->type == DIR_ROOT)
			continue;

		/*
		 * Give any CPUs back to the default group. We cannot copy
		 * cpu_online_mask because a CPU might have executed the
		 * offline callback already, but is still marked online.
		 */
		cpumask_or(&resctrl_default->cpu_mask,
			   &resctrl_default->cpu_mask, &rg->cpu_mask);

		arch_free_resctrl_ids(rg);

		rni = (struct resctrl_node_info *)rg - 1;
		kernfs_remove(rni->kn);
		list_del(&rg->list);

		if (atomic_read(&rni->waitcount) != 0)
			rni->flags |= RESCTRL_DELETED;
		else
			resctrl_group_remove(rni);
	}
	/* Notify online CPUs to update per cpu storage and PQR_ASSOC MSR */
	update_resctrl_ids(cpu_online_mask, resctrl_default);
}
