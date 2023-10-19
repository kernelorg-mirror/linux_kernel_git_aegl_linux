// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2023 Intel Corporation. */

#include <linux/cpuhotplug.h>

#include "internal.h"

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

static int resctrl_online_cpu(unsigned int cpu)
{
	struct resctrl_resource *r;

	mutex_lock(&resctrl_mutex);
	for_each_resource_by_cap(r, domain_size)
		resctrl_domain_add_cpu(cpu, r);
	mutex_unlock(&resctrl_mutex);

	return 0;
}

static int resctrl_offline_cpu(unsigned int cpu)
{
	struct resctrl_resource *r;

	mutex_lock(&resctrl_mutex);
	for_each_resource_by_cap(r, domain_size)
		resctrl_domain_remove_cpu(cpu, r);
	mutex_unlock(&resctrl_mutex);

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
