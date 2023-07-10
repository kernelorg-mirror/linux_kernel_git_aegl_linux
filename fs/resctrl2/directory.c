// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2023 Intel Corporation. */

#include "internal.h"

static struct resctrl_node_info mongroup_header = {
	.type = RESCTRL_MONGROUP
};

bool resctrl_populate_dir(struct kernfs_node *parent_kn, struct resctrl_group *rg)
{
	struct resctrl_resource *r;

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

	for_each_monitor_resource(r)
		if (r->mon_domain_dir)
			resctrl_create_all_domain_files(r, rg);

	return true;
}

static void resctrl_depopulate_dir(struct kernfs_node *parent_kn, struct resctrl_group *rg, struct list_head *h)
{
	struct resctrl_resource *r;
	struct kernfs_node *kn;

	resctrl_remove_task_file(parent_kn, h);
	resctrl_remove_cpus_file(parent_kn, h);
	if ((rg->type == DIR_ROOT || rg->type == DIR_CTRL_MON)) {
		resctrl_remove_schemata_file(parent_kn, h);
		resctrl_remove_mode_file(parent_kn, h);
	}

	kn = kernfs_find_and_get_ns(parent_kn, "mon_groups", NULL);
	if (kn)
		kernfs_remove(kn);

	for_each_monitor_resource(r)
		if (r->mon_domain_dir)
			resctrl_remove_all_domain_files(r, rg, h);
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
			resctrl_remove_all_domain_files(r, rg, h);

	head = &rg->child_list;
	list_for_each_entry_safe(sentry, stmp, head, list) {
		rni = (struct resctrl_node_info *)sentry - 1;
		arch_free_resctrl_ids(sentry);

		for_each_monitor_resource(r)
			if (r->mon_domain_dir)
				resctrl_remove_all_domain_files(r, sentry, h);

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
			resctrl_remove_all_domain_files(r, rg, h);

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

		rni = (struct resctrl_node_info *)rg - 1;
		resctrl_depopulate_dir(rni->kn, rg, h);

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

/**
 * mongrp_reparent() - replace parent CTRL_MON group of a MON group
 * @rg:		the MON group whose parent should be replaced
 * @new_prg:	replacement parent CTRL_MON group for @rg
 * @cpus:	cpumask provided by the caller for use during this call
 *
 * Replaces the parent CTRL_MON group for a MON group, resulting in all member
 * tasks' CLOSID immediately changing to that of the new parent group.
 * Monitoring data for the group is unaffected by this operation.
 */
static void mongrp_reparent(struct resctrl_group *rg,
			    struct resctrl_group *new_prg,
			    cpumask_var_t cpus)
{
	struct resctrl_group *old_prg = rg->parent;

	WARN_ON(rg->type != DIR_MON);
	WARN_ON(new_prg->type != DIR_CTRL_MON);

	/* Nothing to do when simply renaming a MON group. */
	if (old_prg == new_prg)
		return;

	WARN_ON(list_empty(&old_prg->child_list));
	list_move_tail(&rg->list,
		       &new_prg->child_list);

	rg->parent = new_prg;
	arch_update_control_ids(rg, new_prg);

	/* Propagate updated closid to all tasks in this group. */
	resctrl_move_group_tasks(rg, rg, cpus);

	update_resctrl_ids(cpus, NULL);
}

int resctrl_rename(struct kernfs_node *kn, struct kernfs_node *new_parent,
		   const char *new_name)
{
	struct resctrl_node_info *rni, *new_parent_rni;
	struct resctrl_group *new_prg;
	struct resctrl_group *rg;
	cpumask_var_t tmpmask;
	int ret;

	/* Source and target must both be directories. */
	if (kernfs_type(kn) != KERNFS_DIR ||
	    kernfs_type(new_parent) != KERNFS_DIR)
		return -EPERM;

	rni = kn->priv;
	new_parent_rni = new_parent->priv;
	if (!rni || !new_parent_rni)
		return -EPERM;


	/* Release both kernfs active_refs before obtaining resctrl mutex. */
	resctrl_kn_get(rni, kn);
	resctrl_kn_get(new_parent_rni, new_parent);

	mutex_lock(&resctrl_mutex);

	resctrl_last_cmd_clear();

	if ((rni->flags & RESCTRL_DELETED) || (new_parent_rni->flags & RESCTRL_DELETED)) {
		ret = -ENOENT;
		goto out;
	}

	rg = (struct resctrl_group *)&rni->priv;

	if (rg->type != DIR_MON) {
		resctrl_last_cmd_puts("Source must be a MON group\n");
		ret = -EPERM;
		goto out;
	}

	if (new_parent_rni->type != RESCTRL_MONGROUP) {
		resctrl_last_cmd_puts("Destination must be a mon_groups subdirectory\n");
		ret = -EPERM;
		goto out;
	}
	new_parent_rni = new_parent->parent->priv;
	new_prg = (struct resctrl_group *)&new_parent_rni->priv;

	/*
	 * If the MON group is monitoring CPUs, the CPUs must be assigned to the
	 * current parent CTRL_MON group and therefore cannot be assigned to
	 * the new parent, making the move illegal.
	 */
	if (!cpumask_empty(&rg->cpu_mask) && rg->parent != new_prg) {
		resctrl_last_cmd_puts("Cannot move a MON group that monitors CPUs\n");
		ret = -EPERM;
		goto out;
	}

	/*
	 * Allocate the cpumask for use in mongrp_reparent() to avoid the
	 * possibility of failing to allocate it after kernfs_rename() has
	 * succeeded.
	 */
	if (!zalloc_cpumask_var(&tmpmask, GFP_KERNEL)) {
		ret = -ENOMEM;
		goto out;
	}

	/*
	 * Perform all input validation and allocations needed to ensure
	 * mongrp_reparent() will succeed before calling kernfs_rename(),
	 * otherwise it would be necessary to revert this call if
	 * mongrp_reparent() failed.
	 */
	ret = kernfs_rename(kn, new_parent, new_name);
	if (!ret)
		mongrp_reparent(rg, new_prg, tmpmask);

	free_cpumask_var(tmpmask);

out:
	mutex_unlock(&resctrl_mutex);
	resctrl_kn_put(rni, kn);
	resctrl_kn_put(new_parent_rni, new_parent);

	return ret;
}
