// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2023 Intel Corporation. */

#include <asm/cpufeatures.h>

#include "../../internal.h"

DEFINE_STATIC_KEY_FALSE(resctrl_enable_key);
DEFINE_PER_CPU(struct resctrl_per_cpu_state, resctrl_per_cpu_state);

/*
 * Trivial allocator for CLOSIDs. Since h/w only supports a small number,
 * we can keep a bitmap of free CLOSIDs in a single integer.
 *
 * Using a global CLOSID across all resources has some advantages and
 * some drawbacks:
 * + We can simply use a field in the task structure to assign a task to a resource
 *   group.
 * + Context switch code can avoid extra memory references deciding which
 *   CLOSID to load into the PQR_ASSOC MSR
 * - We give up some options in configuring resource groups across multi-socket
 *   systems.
 * - Our choices on how to configure each resource become progressively more
 *   limited as the number of resources grows.
 */
static int arch_ids;
static int closid_free_map;
u64 arch_resctrl_default_ids;

void arch_resctrl_apply_ids(u64 resctrl_ids)
{
	wrmsrl(MSR_IA32_PQR_ASSOC, resctrl_ids);
}

static void closid_init(void)
{
	closid_free_map = BIT_MASK(arch_ids) - 1;

	/* CLOSID 0 is always reserved for the default group */
	closid_free_map &= ~1;
}

static int closid_alloc(void)
{
	u32 closid = ffs(closid_free_map);

	if (closid == 0)
		return -ENOSPC;
	closid--;
	closid_free_map &= ~(1 << closid);

	return closid;
}

void closid_free(int closid)
{
	closid_free_map |= 1 << closid;
}

#define RESCTRL_ID(c, r) (((u64)(c) << 32) | (r))

bool arch_check_resctrl_support(void)
{
	return boot_cpu_has(X86_FEATURE_CQM) || boot_cpu_has(X86_FEATURE_RDT_A);
}

bool arch_init_alloc_ids(struct resctrl_resource *r)
{
	if (r->num_alloc_ids < arch_ids)
		return false;
	if (arch_ids != 0) {
		if (r->num_alloc_ids > arch_ids)
			r->num_alloc_ids = arch_ids;
		return true;
	}
	arch_ids = r->num_alloc_ids;

	closid_init();

	return true;
}

bool arch_alloc_resctrl_ids(struct resctrl_group *rg)
{
	int c, r;

	switch (rg->type) {
	case DIR_CTRL_MON:
		c = closid_alloc();
		if (c < 0)
			return false;
		r = rmid_alloc(-1);
		if (r < 0) {
			closid_free(c);
			return false;
		}
		rg->resctrl_ids = RESCTRL_ID(c, r);
		return true;

	case DIR_MON:
		/* monitor groups have same CLOSID as parent */
		c = rg->parent->resctrl_ids >> 32;
		r = rmid_alloc(rg->parent->resctrl_ids & 0xffff);
		if (r < 0)
			return false;
		rg->resctrl_ids = RESCTRL_ID(c, r);
		return true;

	default:
		return false;
	}
}

void arch_free_resctrl_ids(struct resctrl_group *rg)
{
	closid_free(rg->resctrl_ids >> 32);

	rmid_free(rg->resctrl_ids & 0xffff);
}
