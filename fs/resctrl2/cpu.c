// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2023 Intel Corporation. */

#include <linux/cpuhotplug.h>

#include "internal.h"

static int cpu_seq_show(struct seq_file *m, struct resctrl_group *rg, bool mask)
{
	seq_printf(m, mask ? "%*pb\n" : "%*pbl\n", cpumask_pr_args(&rg->cpu_mask));

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

static int cpu_seq_show_list(struct seq_file *m, struct resctrl_group *rg)
{
	return cpu_seq_show(m, rg, false);
}

static ssize_t cpu_write_list(char *buf, size_t nbytes, struct resctrl_group *rg,
			      struct kernfs_open_file *of)
{
	return nbytes;
}

static int cpu_seq_show_mask(struct seq_file *m, struct resctrl_group *rg)
{
	return cpu_seq_show(m, rg, true);
}

static ssize_t cpu_write_mask(char *buf, size_t nbytes, struct resctrl_group *rg,
			      struct kernfs_open_file *of)
{
	return nbytes;
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

/*
 * Update the resctrl_ids on all cpus in @cpu_mask.
 * Per task resctrl_ids must have been set up before calling this function.
 */
void update_resctrl_ids(const struct cpumask *cpu_mask, struct resctrl_group *r)
{
	on_each_cpu_mask(cpu_mask, update_cpu_resctrl_ids, r, 1);
}

static int resctrl_online_cpu(unsigned int cpu)
{
	struct resctrl_resource *r;

	mutex_lock(&resctrl_mutex);
	for_each_resource_by_cap(r, domain_size)
		resctrl_domain_add_cpu(cpu, r);
	/* The cpu is set in default group after online. */
	cpumask_set_cpu(cpu, &resctrl_default->cpu_mask);
	mutex_unlock(&resctrl_mutex);

	return 0;
}

static int resctrl_offline_cpu(unsigned int cpu)
{
	struct resctrl_resource *r;
	LIST_HEAD(clean_list);

	mutex_lock(&resctrl_mutex);
	for_each_resource_by_cap(r, domain_size)
		resctrl_domain_remove_cpu(cpu, r, &clean_list);
	mutex_unlock(&resctrl_mutex);

	resctrl_node_file_cleanup(&clean_list);

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
