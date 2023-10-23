// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2023 Intel Corporation. */

/*
 *  X86 Resource Control Driver For L3 cache allocation
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/resctrl.h>
#include <linux/seq_file.h>

#include <asm/cpu_device_id.h>

#include "rdt.h"

#undef pr_fmt
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

struct mydomain {
	RESCTRL_DOMAIN_HEADER;
};

static u32 cbm_mask;
static bool sparse_masks;
static int min_cbm_bits = 1;
static unsigned long shareable_bits;

static struct resctrl_resource cat;
#define num_closids cat.num_alloc_ids

static bool checkmask(unsigned long  mask, bool quiet);

static void update_msrs(void *info)
{
	unsigned long *curval = info;
	unsigned long *staged = curval + num_closids;

	for (int i = 0; i < num_closids; i++) {
		if (staged[i] != curval[i]) {
			curval[i] = checkmask(staged[i], true) ? staged[i] : shareable_bits;
			wrmsrl(MSR_IA32_L3_CBM_BASE + i, curval[i]);
		}
	}
}

static void domain_update(struct resctrl_resource *r, int what, int cpu, void *domain)
{
	unsigned int eax, ebx, ecx, edx;
	struct mydomain *m = domain;
	unsigned long *staged;
	u64 cbm_mask;

	if (what == RESCTRL_DOMAIN_ADD || what == RESCTRL_DOMAIN_DELETE) {
		cpuid_count(0x10, 1, &eax, &ebx, &ecx, &edx);
		m->param = (eax & 0x1f) + 1;
		m->share_bits = &shareable_bits;
		cbm_mask = GENMASK_ULL(eax & 0x1f, 0);
		staged = m->ctrls + num_closids;
		for (int i = 0; i < num_closids; i++)
			staged[i] = cbm_mask;
		smp_call_function_single(cpu, update_msrs, m->ctrls, 1);
	}
}

static const struct x86_cpu_id cat_feature[] = {
	X86_MATCH_FEATURE(X86_FEATURE_CAT_L3, 0),
	{ }
};
MODULE_DEVICE_TABLE(x86cpu, cat_feature);

RESCTRL_FILE_DEF(cbm_mask, "%x\n")
RESCTRL_FILE_DEF(min_cbm_bits, "%d\n")
RESCTRL_FILE_DEF(num_closids, "%d\n")
RESCTRL_FILE_DEF(shareable_bits, "%lx\n")
RESCTRL_FILE_DEF(sparse_masks, "%d\n")

static struct resctrl_fileinfo cat_files[] = {
	{
		.name	= "cbm_mask",
		.show	= cbm_mask_show,
	},
	{
		.name	= "min_cbm_bits",
		.show	= min_cbm_bits_show,
	},
	{
		.name	= "num_closids",
		.show	= num_closids_show,
	},
	{
		.name	= "shareable_bits",
		.show	= shareable_bits_show,
	},
	{
		.name	= "sparse_masks",
		.show	= sparse_masks_show,
	},
	{ }
};

static bool checkmask(unsigned long  mask, bool quiet)
{
	unsigned long first_bit, last_bit;

	/* Intel doesn't allow all zero bits */
	if (!mask) {
		if (!quiet)
			resctrl_last_cmd_puts("All zero mask not allowed\n");
		return false;
	}

	if (sparse_masks)
		return true;

	first_bit = __ffs(mask);
	last_bit = __fls(mask);
	if (mask != (((1u << (last_bit + 1)) - 1) & ~((1u << first_bit) - 1))) {
		if (!quiet)
			resctrl_last_cmd_puts("Mask set bits must be consecutive\n");
		return false;
	}

	return true;
}

static bool validate(struct resctrl_resource *r)
{
	struct mydomain *m;

	list_for_each_entry(m, &r->domains, list) {
		unsigned long *curval =  m->ctrls;
		unsigned long *staged = curval + num_closids;

		for (int i = 0; i < num_closids; i++) {
			if (staged[i] != curval[i])
				if (!checkmask(staged[i], false))
					return false;
		}
	}

	return true;
}

static void applychanges(struct resctrl_resource *r)
{
	struct mydomain *m;
	int cpu, i;

	list_for_each_entry(m, &r->domains, list) {
		unsigned long *curval =  m->ctrls;
		unsigned long *staged = curval + num_closids;

		for (i = 0; i < num_closids; i++)
			if (staged[i] != curval[i])
				break;
		if (i != num_closids) {
			cpu = cpumask_first(&m->cpu_mask);
			smp_call_function_single(cpu, update_msrs, m->ctrls, 1);
		}
	}
}

static struct resctrl_resource cat = {
	.name		= "L3",
	.scope		= RESCTRL_L3CACHE,
	.domain_size	= sizeof(struct mydomain),
	.domains	= LIST_HEAD_INIT(cat.domains),
	.domain_update	= domain_update,
	.schemata_name	= "L3",
	.schemata_fmt	= RESCTRL_BITMASK,
	.schemata_validate = validate,
	.applychanges	= applychanges,
	.infodir	= "L3",
	.infofiles	= cat_files,
};

static int __init cat_init(void)
{
	unsigned int eax, ebx, ecx, edx;
	int ret;

	if (!boot_cpu_has(X86_FEATURE_RDT_A)) {
		pr_debug("No RDT allocation support\n");
		return -ENODEV;
	}
	if (!x86_match_cpu(cat_feature)) {
		pr_debug("No support for CAT L3\n");
		return -ENODEV;
	}

	cpuid_count(0x10, 1, &eax, &ebx, &ecx, &edx);
	num_closids = (edx + 1);
	cbm_mask = GENMASK_ULL(eax & 0x1f, 0);
	shareable_bits = ebx;

	if ((boot_cpu_data.x86_vendor == X86_VENDOR_INTEL && (ecx & BIT(3))) ||
	    boot_cpu_data.x86_vendor == X86_VENDOR_AMD)
		sparse_masks = true;

	ret = resctrl_register_resource(&cat);

	if (ret == 0 && cat.num_alloc_ids < edx + 1) {
		pr_info("Core is only using %d of %d supported CLOSID\b", cat.num_alloc_ids,
			edx + 1);
	}

	return ret;
}

static void __exit cat_cleanup(void)
{
	resctrl_unregister_resource(&cat);
}

module_init(cat_init);
module_exit(cat_cleanup);

MODULE_AUTHOR("Tony Luck <tony.luck@intel.com>");
MODULE_IMPORT_NS(RESCTRL);
MODULE_LICENSE("GPL");
