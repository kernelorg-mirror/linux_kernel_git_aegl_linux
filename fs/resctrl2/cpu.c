// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2023 Intel Corporation. */

#include <linux/cpuhotplug.h>

#include "internal.h"

static int cpu_seq_show(struct seq_file *m, struct resctrl_group *rg, bool mask)
{
	seq_printf(m, mask ? "%*pb\n" : "%*pbl\n",
		   cpumask_pr_args(&rg->cpu_mask));

	return 0;
}

/*
 * This is safe against resctrl_sched_in() called from __switch_to()
 * because __switch_to() is executed with interrupts disabled. A local call
 * from update_resctrl_ids() is protected against __switch_to() because
 * preemption is disabled.
 */
static void update_cpu_resctrl_ids(void *info)
{
	struct resctrl_group *r = info;

	if (r)
		this_cpu_write(resctrl_per_cpu_state.default_resctrl_ids, r->resctrl_ids);

	/*
	 * Re-use the context switch code, current running
	 * task may have its own reasctrl_ids selected.
	 */
	resctrl_sched_in(current);
}

/*
 * Update the resctrl_ids on all cpus in @cpu_mask.
 * Per task resctrl_ids must have been set up before calling this function.
 */
void update_resctrl_ids(const struct cpumask *cpu_mask, struct resctrl_group *r)
{
	on_each_cpu_mask(cpu_mask, update_cpu_resctrl_ids, r, 1);
}

static void cpumask_resctrl_group_clear(struct resctrl_group *r, struct cpumask *m)
{
	struct resctrl_group *crgrp;

	cpumask_andnot(&r->cpu_mask, &r->cpu_mask, m);
	/* update the child mon group masks as well*/
	list_for_each_entry(crgrp, &r->child_list, list)
		cpumask_and(&crgrp->cpu_mask, &r->cpu_mask, &crgrp->cpu_mask);
}

static int cpus_ctrl_write(struct resctrl_group *rg, cpumask_var_t newmask,
			   cpumask_var_t tmpmask, cpumask_var_t tmpmask1)
{
	struct resctrl_group *r, *crgrp;
	struct list_head *head;

	/* Check whether cpus are dropped from this group */
	cpumask_andnot(tmpmask, &rg->cpu_mask, newmask);
	if (!cpumask_empty(tmpmask)) {
		/* Can't drop from default group */
		if (rg->type == DIR_ROOT) {
			resctrl_last_cmd_puts("Can't drop CPUs from default group\n");
			return -EINVAL;
		}

		/* Give any dropped cpus to resctrl_default */
		cpumask_or(&resctrl_default->cpu_mask,
			   &resctrl_default->cpu_mask, tmpmask);
		update_resctrl_ids(tmpmask, resctrl_default);
	}

	/*
	 * If we added cpus, remove them from previous group and
	 * the prev group's child groups that owned them
	 * and update per-cpu resctrl_ids.
	 */
	cpumask_andnot(tmpmask, newmask, &rg->cpu_mask);
	if (!cpumask_empty(tmpmask)) {
		list_for_each_entry(r, &all_ctrl_groups, list) {
			if (r == rg)
				continue;
			cpumask_and(tmpmask1, &r->cpu_mask, tmpmask);
			if (!cpumask_empty(tmpmask1))
				cpumask_resctrl_group_clear(r, tmpmask1);
		}
		update_resctrl_ids(tmpmask, rg);
	}

	/* Done pushing/pulling - update this group with new mask */
	cpumask_copy(&rg->cpu_mask, newmask);

	/*
	 * Clear child mon group masks since there is a new parent mask
	 * now and update the resctrl_ids for the cpus the child lost.
	 */
	head = &rg->child_list;
	list_for_each_entry(crgrp, head, list) {
		cpumask_and(tmpmask, &rg->cpu_mask, &crgrp->cpu_mask);
		update_resctrl_ids(tmpmask, rg);
		cpumask_clear(&crgrp->cpu_mask);
	}

	return 0;
}

static int cpus_mon_write(struct resctrl_group *rg, cpumask_var_t newmask,
			  cpumask_var_t tmpmask)
{
	struct resctrl_group *prgrp = rg->parent, *crgrp;
	struct list_head *head;

	/* Check whether cpus belong to parent ctrl group */
	cpumask_andnot(tmpmask, newmask, &prgrp->cpu_mask);
	if (!cpumask_empty(tmpmask)) {
		resctrl_last_cmd_puts("Can only add CPUs to mongroup that belong to parent\n");
		return -EINVAL;
	}

	/* Check whether cpus are dropped from this group */
	cpumask_andnot(tmpmask, &rg->cpu_mask, newmask);
	if (!cpumask_empty(tmpmask)) {
		/* Give any dropped cpus to parent group */
		cpumask_or(&prgrp->cpu_mask, &prgrp->cpu_mask, tmpmask);
		update_resctrl_ids(tmpmask, prgrp);
	}

	/*
	 * If we added cpus, remove them from previous group that owned them
	 * and update per-cpu resctrl_ids
	 */
	cpumask_andnot(tmpmask, newmask, &rg->cpu_mask);
	if (!cpumask_empty(tmpmask)) {
		head = &prgrp->child_list;
		list_for_each_entry(crgrp, head, list) {
			if (crgrp == rg)
				continue;
			cpumask_andnot(&crgrp->cpu_mask, &crgrp->cpu_mask,
				       tmpmask);
		}
		update_resctrl_ids(tmpmask, rg);
	}

	/* Done pushing/pulling - update this group with new mask */
	cpumask_copy(&rg->cpu_mask, newmask);

	return 0;
}

static ssize_t cpu_write(char *buf, size_t nbytes, struct resctrl_group *rg, bool mask)
{
	cpumask_var_t tmpmask, newmask, tmpmask1;
	int ret;

	if (!buf)
		return -EINVAL;

	if (!zalloc_cpumask_var(&tmpmask, GFP_KERNEL))
		return -ENOMEM;
	if (!zalloc_cpumask_var(&newmask, GFP_KERNEL)) {
		free_cpumask_var(tmpmask);
		return -ENOMEM;
	}
	if (!zalloc_cpumask_var(&tmpmask1, GFP_KERNEL)) {
		free_cpumask_var(tmpmask);
		free_cpumask_var(newmask);
		return -ENOMEM;
	}

	resctrl_last_cmd_clear();


	if (mask)
		ret = cpumask_parse(buf, newmask);
	else
		ret = cpulist_parse(buf, newmask);

	if (ret) {
		resctrl_last_cmd_puts("Bad CPU list/mask\n");
		goto done;
	}

	/* check that user didn't specify any offline cpus */
	cpumask_andnot(tmpmask, newmask, cpu_online_mask);
	if (!cpumask_empty(tmpmask)) {
		ret = -EINVAL;
		resctrl_last_cmd_puts("Can only assign online CPUs\n");
		goto done;
	}

	if (rg->type == DIR_ROOT || rg->type == DIR_CTRL_MON)
		ret = cpus_ctrl_write(rg, newmask, tmpmask, tmpmask1);
	else if (rg->type == DIR_MON)
		ret = cpus_mon_write(rg, newmask, tmpmask);
	else
		ret = -EINVAL;

done:
	free_cpumask_var(tmpmask);
	free_cpumask_var(newmask);
	free_cpumask_var(tmpmask1);

	return ret ?: nbytes;
}

static int cpu_seq_show_list(struct seq_file *m, struct resctrl_group *rg)
{
	return cpu_seq_show(m, rg, false);
}

static ssize_t cpu_write_list(char *buf, size_t nbytes, struct resctrl_group *rg,
			      struct kernfs_open_file *of)
{
	return cpu_write(buf, nbytes, rg, false);
}

static int cpu_seq_show_mask(struct seq_file *m, struct resctrl_group *rg)
{
	return cpu_seq_show(m, rg, true);
}

static ssize_t cpu_write_mask(char *buf, size_t nbytes, struct resctrl_group *rg,
			      struct kernfs_open_file *of)
{
	return cpu_write(buf, nbytes, rg, true);
}

bool resctrl_add_cpus_file(struct kernfs_node *parent_kn)
{
	struct resctrl_node_info *rni, *prni;
	struct core_file_info *cfi;

	rni = resctrl_add_file(parent_kn, "cpus", 0644, RESCTRL_COREFILE);
	if (!rni)
		return false;
	prni = parent_kn->priv;
	cfi = (struct core_file_info *)&rni->priv;
	cfi->rg = (struct resctrl_group *)&prni->priv;
	cfi->show = cpu_seq_show_mask;
	cfi->write = cpu_write_mask;

	rni = resctrl_add_file(parent_kn, "cpus_list", 0644, RESCTRL_COREFILE);
	if (!rni)
		return false;
	prni = parent_kn->priv;
	cfi = (struct core_file_info *)&rni->priv;
	cfi->rg = (struct resctrl_group *)&prni->priv;
	cfi->show = cpu_seq_show_list;
	cfi->write = cpu_write_list;

	return true;
}

void resctrl_remove_cpus_file(struct kernfs_node *parent_kn, struct list_head *h)
{
	resctrl_remove_file("cpus", parent_kn, h);
	resctrl_remove_file("cpus_list", parent_kn, h);
}

static void reset_resctrl_ids(void)
{
	struct resctrl_per_cpu_state *state = this_cpu_ptr(&resctrl_per_cpu_state);

	state->cached_resctrl_ids = arch_resctrl_default_ids;
	state->default_resctrl_ids = arch_resctrl_default_ids;

	arch_resctrl_apply_ids(arch_resctrl_default_ids);
}

static int resctrl_online_cpu(unsigned int cpu)
{
	struct resctrl_resource *r;

	mutex_lock(&resctrl_mutex);
	for_each_resource(r)
		if (r->domain_size)
			resctrl_domain_add_cpu(cpu, r);
	/* The cpu is set in default group after online. */
	cpumask_set_cpu(cpu, &resctrl_default->cpu_mask);
	reset_resctrl_ids();
	mutex_unlock(&resctrl_mutex);

	return 0;
}

static void clear_childcpus(struct resctrl_group *rg, unsigned int cpu)
{
	struct resctrl_group *crg;

	list_for_each_entry(crg, &rg->child_list, list) {
		if (cpumask_test_and_clear_cpu(cpu, &crg->cpu_mask))
			break;
	}
}

static int resctrl_offline_cpu(unsigned int cpu)
{
	LIST_HEAD(file_clean_list);
	struct resctrl_resource *r;
	struct resctrl_group *rg;

	mutex_lock(&resctrl_mutex);
	for_each_resource(r)
		if (r->domain_size)
			resctrl_domain_remove_cpu(cpu, r, &file_clean_list);
	list_for_each_entry(rg, &all_ctrl_groups, list) {
		if (cpumask_test_and_clear_cpu(cpu, &rg->cpu_mask)) {
			clear_childcpus(rg, cpu);
			break;
		}
	}
	reset_resctrl_ids();
	mutex_unlock(&resctrl_mutex);

	resctrl_node_file_cleanup(&file_clean_list);

	return 0;
}

static enum cpuhp_state cpu_hp_state;

int resctrl_cpu_init(void)
{
	cpu_hp_state = cpuhp_setup_state(CPUHP_AP_ONLINE_DYN,
					 "resctrl2/cpu:online",
					 resctrl_online_cpu, resctrl_offline_cpu);
	return cpu_hp_state;
}

void resctrl_cpu_exit(void)
{
	cpuhp_remove_state(cpu_hp_state);
}
