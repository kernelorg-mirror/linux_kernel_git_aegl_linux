// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2023 Intel Corporation. */

#include <linux/bitfield.h>
#include <linux/bits.h>

#include <asm/cpufeatures.h>

#include "../../internal.h"

#define CLOSID_FIELD	GENMASK_ULL(63, 32)
#define RMID_FIELD	GENMASK_ULL(31, 0)

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
static u32 closid_free_map;

// Default CLOSID=0 / RMID=0 for root resctrl group
resctrl_ids_t arch_resctrl_default_ids;

static int closid_alloc(void)
{
	int closid = ffs(closid_free_map);

	if (closid == 0)
		return -ENOSPC;
	closid--;
	closid_free_map &= ~BIT(closid);

	return closid;
}

static void closid_free(int closid)
{
	closid_free_map |= BIT(closid);
}

static resctrl_ids_t resctrl_id(int closid, int rmid)
{
	return FIELD_PREP(CLOSID_FIELD, closid) | FIELD_PREP(RMID_FIELD, rmid);
}

bool arch_init_alloc_ids(struct resctrl_resource *r)
{
	int i;

	/* Allocator already initialized for same number of IDs */
	if (r->num_alloc_ids == arch_ids)
		return true;

	/*
	 * This resource supports fewer IDs. Check if any out of range
	 * IDs are already allocated. Fail if any is in use.
	 */
	if (r->num_alloc_ids < arch_ids) {
		for (i = r->num_alloc_ids; i < arch_ids; i++)
			if (!(closid_free_map & BIT(i)))
				return false;

		pr_info("Reducing available CLOSIDs from %d to %d\n", arch_ids, r->num_alloc_ids);
		/* Remove IDs that this resource cannot use */
		for (i = r->num_alloc_ids; i < arch_ids; i++)
			closid_free_map &= ~BIT(i);
		arch_ids = r->num_alloc_ids;

		return true;
	}

	/*
	 * This resource supports more IDs than previously registered
	 * resources. Tell the resource the extras will not be used.
	 */
	if (arch_ids != 0) {
		r->num_alloc_ids = arch_ids;
		return true;
	}

	/* This is first module to declare how many IDs */
	if (r->num_alloc_ids > sizeof(closid_free_map) * BITS_PER_BYTE)
		return false;

	arch_ids = r->num_alloc_ids;
	closid_free_map = BIT_MASK(arch_ids) - 1;

	/* CLOSID 0 is always reserved for the default group */
	closid_free_map &= ~BIT(0);

	return true;
}

void arch_reset_alloc_ids(void)
{
	arch_ids = 0;
}

bool arch_alloc_resctrl_ids(struct resctrl_group *rg)
{
	int c;

	if (rg->type == DIR_CTRL_MON) {
		c = closid_alloc();
		if (c < 0)
			return false;
		rg->resctrl_ids = resctrl_id(c, 0);
		return true;
	}

	return false;
}

void arch_free_resctrl_ids(struct resctrl_group *rg)
{
	if (rg->type == DIR_CTRL_MON)
		closid_free(FIELD_GET(CLOSID_FIELD, rg->resctrl_ids));
}
