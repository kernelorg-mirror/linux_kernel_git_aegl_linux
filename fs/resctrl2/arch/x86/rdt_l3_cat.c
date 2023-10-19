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
	int			cbm_len;
};

static u32 cbm_mask;
static int min_cbm_bits = 1;
static u32 shareable_bits;

static struct resctrl_resource cat;
#define num_closids cat.num_alloc_ids

static void domain_update(struct resctrl_resource *r, int what, int cpu, void *domain)
{
	unsigned int eax, ebx, ecx, edx;
	struct mydomain *m = domain;

	if (what == RESCTRL_DOMAIN_ADD) {
		cpuid_count(0x10, 1, &eax, &ebx, &ecx, &edx);
		m->cbm_len = eax & 0x1f;
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
RESCTRL_FILE_DEF(shareable_bits, "%x\n")

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
	{ }
};

static struct resctrl_resource cat = {
	.name		= "L3",
	.scope		= RESCTRL_L3CACHE,
	.domain_size	= sizeof(struct mydomain),
	.domains	= LIST_HEAD_INIT(cat.domains),
	.domain_update	= domain_update,
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
	cbm_mask = (1u << ((eax & 0x1f) + 1)) - 1;
	shareable_bits = ebx;

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
