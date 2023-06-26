// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2023 Intel Corporation. */

#include "internal.h"

static struct resctrl_group mongroup_header = {
	.type = DIR_MONGROUP
};

static struct resctrl_group mondata_header = {
	.type = DIR_MONDATA
};

void resctrl_create_domain_files(struct kernfs_node *parent_kn, struct resctrl_resource *r,
				 struct resctrl_group *rg)
{
	struct resctrl_domain *d;
	struct kernfs_node *kn;
	char name[20];

	list_for_each_entry(d, &r->domains, list) {
		sprintf(name, r->mon_domain_dir, d->id);
		kn = kernfs_find_and_get_ns(parent_kn, name, NULL);
		if (!kn)
			kn = resctrl_add_dir(parent_kn, name, (void *)(long)d->id);
		resctrl_add_file(kn, r->mon_domain_file, 0444, r->mod_domain_ops,
				 (void *)rg->resctrl_ids);
	}
	kernfs_activate(parent_kn);
}

void resctrl_remove_domain_files(struct kernfs_node *parent_kn, struct resctrl_resource *r,
				 struct resctrl_group *rg)
{
	struct resctrl_domain *d;
	struct kernfs_node *kn;
	char name[20];

	list_for_each_entry(d, &r->domains, list) {
		sprintf(name, r->mon_domain_dir, d->id);
		kn = kernfs_find_and_get_ns(parent_kn, name, NULL);
		kn = kernfs_find_and_get_ns(kn, r->mon_domain_file, NULL);
		kernfs_remove(kn);
	}
}

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

void resctrl_group_remove(struct resctrl_group *rg)
{
	kernfs_put(rg->kn);
	kfree(rg);
}

int resctrl_mkdir(struct kernfs_node *parent_kn, const char *name, umode_t mode)
{
	struct resctrl_group *rg, *prg;
	struct resctrl_resource *r;
	struct kernfs_node *kn;
	int ret = 0;

	if (strchr(name, '\n'))
		return -EINVAL;

	rg = kzalloc(sizeof(*rg), GFP_KERNEL);
	if (!rg)
		return -ENOMEM;

	prg = resctrl_group_kn_lock_live(parent_kn);
	if (!prg) {
		kfree(rg);
		ret = -ENOENT;
		goto unlock;
	}

	switch (prg->type) {
	case DIR_ROOT:
		rg->type = DIR_CTRL_MON;
		rg->parent = kernfs_to_resctrl_group(parent_kn);
		rg->mode = RESCTRL_SHARED;
		if (!arch_alloc_resctrl_ids(rg)) {
			kfree(rg);
			ret = -ENOSPC;
			goto unlock;
		}

		for_each_control_resource(r)
			if (r->setmode)
				r->setmode(r, rg->resctrl_ids, RESCTRL_SHARED);

		list_add(&rg->list, &all_ctrl_groups);
		INIT_LIST_HEAD(&rg->child_list);
		break;
	case DIR_MONGROUP:
		rg->type = DIR_MON;
		rg->parent = kernfs_to_resctrl_group(parent_kn->parent);
		if (!arch_alloc_resctrl_ids(rg)) {
			kfree(rg);
			ret = -ENOSPC;
			goto unlock;
		}
		list_add(&rg->list, &rg->parent->child_list);
		break;
	default:
		kfree(rg);
		ret = -EPERM;
		goto unlock;
	}

	kn = resctrl_add_dir(parent_kn, name, rg);
	if (!kn) {
		list_del(&rg->list);
		kfree(rg);
		ret = -EINVAL;
		goto unlock;
	}
	rg->kn = kn;
	kernfs_get(kn);

	resctrl_populate_dir(kn, rg);

	kernfs_activate(kn);
unlock:
	resctrl_group_kn_unlock(parent_kn);

	return ret;
}

static void free_all_child_resctrlgrp(struct resctrl_group *rg)
{
	struct resctrl_group *sentry, *stmp;
	struct list_head *head;

	head = &rg->child_list;
	list_for_each_entry_safe(sentry, stmp, head, list) {
		arch_free_resctrl_ids(sentry);
		list_del(&sentry->list);

		if (atomic_read(&sentry->waitcount) != 0)
			sentry->flags = RESCTRL_DELETED;
		else
			resctrl_group_remove(sentry);
	}
}

static void resctrl_rmdir_ctrl(struct resctrl_group *rg, struct cpumask *mask)
{
	struct resctrl_resource *r;
	int cpu;

	/* Give any tasks back to the default group */
	resctrl_move_group_tasks(rg, rg->parent, mask);

	/* Give any CPUs back to the default group */
	cpumask_or(&resctrl_default.cpu_mask,
		   &resctrl_default.cpu_mask, &rg->cpu_mask);

	/* Update resctrl_ids of the moved CPUs first */
	for_each_cpu(cpu, &rg->cpu_mask)
		per_cpu(resctrl_per_cpu_state.default_resctrl_ids, cpu) = arch_resctrl_default_ids;

	/*
	 * Update the MSR on moved CPUs and CPUs which have moved
	 * task running on them.
	 */
	cpumask_or(mask, mask, &rg->cpu_mask);
	update_resctrl_ids(mask, NULL);

	/*
	 * Free all the child monitor groups.
	 */
	free_all_child_resctrlgrp(rg);

	for_each_control_resource(r)
		if (r->setmode)
			r->setmode(r, rg->resctrl_ids, RESCTRL_FREE);

	arch_free_resctrl_ids(rg);
	list_del(&rg->list);

	rg->flags = RESCTRL_DELETED;
	kernfs_remove(rg->kn);
}

static void resctrl_rmdir_mon(struct resctrl_group *rg, struct cpumask *mask)
{
	struct resctrl_group *prg = rg->parent;
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

	rg->flags = RESCTRL_DELETED;
	arch_free_resctrl_ids(rg);

	/*
	 * Remove the group from parent's list of children
	 */
	WARN_ON(list_empty(&prg->child_list));
	list_del(&rg->list);

	kernfs_remove(rg->kn);
}

int resctrl_rmdir(struct kernfs_node *kn)
{
	struct resctrl_group *rg;
	cpumask_var_t tmpmask;
	int ret = 0;

	if (!zalloc_cpumask_var(&tmpmask, GFP_KERNEL))
		return -ENOMEM;
	rg = resctrl_group_kn_lock_live(kn);
	if (!rg || (rg->type != DIR_CTRL_MON && rg->type != DIR_MON)) {
		ret = -EPERM;
		goto out;
	}

	if (rg->type == DIR_CTRL_MON)
		resctrl_rmdir_ctrl(rg, tmpmask);
	else
		resctrl_rmdir_mon(rg, tmpmask);

out:
	resctrl_group_kn_unlock(kn);
	free_cpumask_var(tmpmask);

	return ret;
}

void resctrl_rmdir_all_sub(void)
{
	struct resctrl_group *rg, *tmp;

	list_for_each_entry_safe(rg, tmp, &all_ctrl_groups, list) {
		/* Free any child resource ids */
		free_all_child_resctrlgrp(rg);

		/* Remove each group other than root */
		if (rg->type == DIR_ROOT)
			continue;

		/*
		 * Give any CPUs back to the default group. We cannot copy
		 * cpu_online_mask because a CPU might have executed the
		 * offline callback already, but is still marked online.
		 */
		cpumask_or(&resctrl_default.cpu_mask,
			   &resctrl_default.cpu_mask, &rg->cpu_mask);

		arch_free_resctrl_ids(rg);

		kernfs_remove(rg->kn);
		list_del(&rg->list);

		if (atomic_read(&rg->waitcount) != 0)
			rg->flags = RESCTRL_DELETED;
		else
			resctrl_group_remove(rg);
	}
	/* Notify online CPUs to update per cpu storage and PQR_ASSOC MSR */
	update_resctrl_ids(cpu_online_mask, &resctrl_default);

#if 0
	kernfs_remove(kn_info);
	kernfs_remove(kn_mongrp);
	kernfs_remove(kn_mondata);
#endif
}
