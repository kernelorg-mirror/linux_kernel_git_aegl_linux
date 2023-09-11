// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2023 Intel Corporation. */

#include <linux/cpuhotplug.h>

#include "internal.h"

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
