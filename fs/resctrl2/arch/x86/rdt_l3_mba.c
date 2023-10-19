// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2023 Intel Corporation. */

/*
 *  X86 Resource Control Driver For L3 memory bandwidth allocation
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

static int bandwidth_gran, delay_linear, min_bandwidth;
static bool supports_resource_aware;
static int resource_aware;
static struct resctrl_resource mba;
#define num_closids mba.num_alloc_ids

#define ENABLE_RESOURCE_AWARE	BIT(2)

static void update_msrs(void *info)
{
	unsigned long *curval = info;
	unsigned long *staged = curval + num_closids;

	for (int i = 0; i < num_closids; i++) {
		if (staged[i] != curval[i]) {
			curval[i] = staged[i];
			wrmsrl(MSR_IA32_MBA_THRTL_BASE + i, 100 - curval[i]);
		}
	}
}

static void update_resource_aware(void *info)
{
	u64 msr;

	rdmsrl(MSR_IA32_MBA_CFG, msr);
	if (resource_aware)
		msr |= ENABLE_RESOURCE_AWARE;
	else
		msr &= ~ENABLE_RESOURCE_AWARE;
	wrmsrl(MSR_IA32_MBA_CFG, msr);
}

static void domain_update(struct resctrl_resource *r, int what, int cpu, void *domain)
{
	unsigned int eax, ebx, ecx, edx;
	struct mydomain *m = domain;
	unsigned long *curval;
	unsigned long *staged;

	if (what == RESCTRL_DOMAIN_ADD || what == RESCTRL_DOMAIN_DELETE) {
		cpuid_count(0x10, 3, &eax, &ebx, &ecx, &edx);
		m->param = 100;
		curval = m->ctrls;
		staged = m->ctrls + num_closids;
		for (int i = 0; i < num_closids; i++) {
			curval[i] = 0;
			staged[i] = 100;
		}
		smp_call_function_single(cpu, update_msrs, m->ctrls, 1);

		if (supports_resource_aware)
			smp_call_function_single(cpu, update_resource_aware, NULL, 1);
	}
}

static const struct x86_cpu_id mba_feature[] = {
	X86_MATCH_FEATURE(X86_FEATURE_MBA, 0),
	{ }
};
MODULE_DEVICE_TABLE(x86cpu, mba_feature);

static const struct x86_cpu_id resource_aware_cpus[] = {
	X86_MATCH_INTEL_FAM6_MODEL(GRANITERAPIDS_X, 0),
	X86_MATCH_INTEL_FAM6_MODEL(ATOM_CRESTMONT_X, 0),
	{ }
};

RESCTRL_FILE_DEF(bandwidth_gran, "%d\n")
RESCTRL_FILE_DEF(delay_linear, "%d\n")
RESCTRL_FILE_DEF(num_closids, "%d\n")
RESCTRL_FILE_DEF(min_bandwidth, "%x\n")
RESCTRL_FILE_DEF(resource_aware, "%d\n")

static ssize_t resource_aware_write(char *buf, size_t nbytes)
{
	unsigned int newval;
	struct mydomain *m;
	int ret, cpu;

	ret = kstrtouint(buf, 0, &newval);
	if (ret || newval > 1)
		return -EINVAL;

	if (newval != resource_aware) {
		resource_aware = newval;
		list_for_each_entry(m, &mba.domains, list) {
			cpu = cpumask_first(&m->cpu_mask);
			smp_call_function_single(cpu, update_resource_aware, NULL, 1);
		}
	}

	return nbytes;
}

static struct resctrl_fileinfo mba_files[] = {
	{
		.name	= "bandwidth_gran",
		.show	= bandwidth_gran_show,
	},
	{
		.name	= "delay_linear",
		.show	= delay_linear_show,
	},
	{
		.name	= "num_closids",
		.show	= num_closids_show,
	},
	{
		.name	= "min_bandwidth",
		.show	= min_bandwidth_show,
	},
	{
		.name	= "resource_aware",
		.show	= resource_aware_show,
	},
	{ }
};

static void checkthrottle(unsigned long  *throttle)
{
	/* Value can't be zero */
	if (!*throttle)
		*throttle = bandwidth_gran;

	/* Value must be multiple of granularity */
	*throttle = roundup(*throttle, bandwidth_gran);
}

static bool validate(struct resctrl_resource *r)
{
	struct mydomain *m;

	list_for_each_entry(m, &r->domains, list) {
		unsigned long *curval =  m->ctrls;
		unsigned long *staged = curval + num_closids;

		for (int i = 0; i < num_closids; i++) {
			if (staged[i] != curval[i])
				checkthrottle(&staged[i]);
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

static struct resctrl_resource mba = {
	.name		= "MB",
	.scope		= RESCTRL_L3CACHE,
	.domain_size	= sizeof(struct mydomain),
	.domains	= LIST_HEAD_INIT(mba.domains),
	.domain_update	= domain_update,
	.schemata_name	= "MB",
	.schemata_fmt	= RESCTRL_ULONG,
	.schemata_validate = validate,
	.applychanges	= applychanges,
	.infodir	= "MB",
	.infofiles	= mba_files,
};

static int __init mba_init(void)
{
	unsigned int eax, ebx, ecx, edx;
	int ret;

	if (!boot_cpu_has(X86_FEATURE_RDT_A)) {
		pr_debug("No RDT allocation support\n");
		return -ENODEV;
	}
	if (!x86_match_cpu(mba_feature)) {
		pr_debug("No support for MBA L3\n");
		return -ENODEV;
	}

	if (x86_match_cpu(resource_aware_cpus)) {
		supports_resource_aware = true;
		mba_files[4].write = resource_aware_write;
	}

	cpuid_count(0x10, 3, &eax, &ebx, &ecx, &edx);
	num_closids = (edx + 1);
	delay_linear = !!(ecx & BIT(2));
	bandwidth_gran = 100 - ((eax & 0xfff) + 1);

	ret = resctrl_register_resource(&mba);

	if (ret == 0 && mba.num_alloc_ids < edx + 1) {
		pr_info("Core is only using %d of %d supported CLOSID\b", mba.num_alloc_ids,
			edx + 1);
	}

	return ret;
}

static void __exit mba_cleanup(void)
{
	resctrl_unregister_resource(&mba);
}

module_init(mba_init);
module_exit(mba_cleanup);

MODULE_AUTHOR("Tony Luck <tony.luck@intel.com>");
MODULE_IMPORT_NS(RESCTRL);
MODULE_LICENSE("GPL");
