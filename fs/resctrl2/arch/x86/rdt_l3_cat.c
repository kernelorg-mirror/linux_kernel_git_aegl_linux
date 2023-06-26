// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2023 Intel Corporation. */

/*
 *  X86 Resource Control Driver For L2 and L3 cache allocation
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/resctrl.h>
#include <linux/seq_file.h>

#include "rdt.h"

#undef pr_fmt
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#ifdef CDP
#define SUFFIX_D	"DATA"
#define SUFFIX_C	"CODE"
#define MULDIV		2
#else
#define SUFFIX_D	""
#define SUFFIX_C	""
#define MULDIV		1
#endif

#if CACHE_LEVEL == 3
#define NAME		"L3"
#define MSR		MSR_IA32_L3_CBM_BASE
#define MSRCDP		MSR_IA32_L3_QOS_CFG
#define LEAF_BIT	1
#define SCOPE		RESCTRL_L3CACHE
#elif CACHE_LEVEL == 2
#define NAME		"L2"
#define MSR		MSR_IA32_L2_CBM_BASE
#define MSRCDP		MSR_IA32_L2_QOS_CFG
#define LEAF_BIT	2
#define SCOPE		RESCTRL_L2CACHE
#else
#error "CACHE_LEVEL not defined"
#endif

struct cbm_masks {
	u64	now;
	u64	staged;
	bool	need_update;
};

struct mydomain {
	int			cbm_len;
	u64			exclusive_mask;
	struct cbm_masks	cbm_masks[];
};
#define get_mydomain(d) ((struct mydomain *)&d[1])

static struct resctrl_resource cat;

static u32 cbm_mask;
static int min_cbm_bits = 1;
static int num_closids;
static u32 shareable_bits;
static bool arch_has_sparse_bitmaps;
static enum resctrl_mode *state;

static void show(struct resctrl_resource *r, struct seq_file *m, u64 resctrl_ids)
{
	int closid = (resctrl_ids >> 32);
	struct resctrl_domain *d;
	struct cbm_masks *cbm;
	char *sep = "";

	list_for_each_entry(d, &r->domains, list) {
		cbm = get_mydomain(d)->cbm_masks;
		seq_printf(m, "%s%d=%llx", sep, d->id, cbm[closid].now);
		sep = ";";
	}
	seq_puts(m, "\n");
}

static void resetstaging(struct resctrl_resource *r, u64 resctrl_ids)
{
	int closid = (resctrl_ids >> 32);
	struct resctrl_domain *d;
	struct cbm_masks *cbm;

	list_for_each_entry(d, &r->domains, list) {
		cbm = get_mydomain(d)->cbm_masks;
		cbm[closid].need_update = false;
	}
}

static bool validate_mask(struct resctrl_domain *d, char *buf, struct cbm_masks *c, int closid)
{
	unsigned long first_bit, last_bit, val;
	struct mydomain *m = get_mydomain(d);
	u64 new_bits, busy_bits;
	int ret;

	ret = kstrtoul(buf, 16, &val);
	if (ret) {
		// rdt_last_cmd_printf("Non-hex character in the mask %s\n", buf);
		return false;
	}

	/* User didn't change this value */
	if (val == c[closid].now)
		return true;

	if ((min_cbm_bits > 0 && val == 0) || val > (1u << (m->cbm_len + 1)) - 1) {
		// rdt_last_cmd_puts("Mask out of range\n");
		return false;
	}
	if (val == 0)
		goto ok;
	first_bit = __ffs(val);
	last_bit = __fls(val);
	if ((last_bit - first_bit) + 1 < min_cbm_bits) {
		// rdt_last_cmd_printf("Need at least %d bits in the mask\n", min_cbm_bits);
		return false;
	}
	if (!arch_has_sparse_bitmaps && val != (((1u << (last_bit + 1)) - 1) & ~((1u << first_bit) - 1))) {
		// rdt_last_cmd_printf("The mask %lx has non-consecutive 1-bits\n", val);
		return false;
	}

	// Only need exclusive mode checks if there are any exclusive groups
	// and change is trying to add bits to this group
	new_bits = val & ~c[closid].now;
	if (!new_bits || !m->exclusive_mask)
		goto ok;

	// Can't claim new bits from existing exclusive set
	if (new_bits & m->exclusive_mask)
		return false;

	// Exclusive groups can only claim extra unused bits
	busy_bits = shareable_bits;
	for (int i = 0; i < num_closids; i++) {
		if (state[i] == RESCTRL_SHARED && i != closid)
			busy_bits |= c[i].now;
	}
	if ((state[closid] == RESCTRL_EXCLUSIVE) && (new_bits & busy_bits))
		return false;

ok:
	c[closid].need_update = true;
	c[closid].staged = val;

	return true;
}

static int parse(struct resctrl_resource *r, char *line, u64 resctrl_ids)
{
	int closid = (resctrl_ids >> 32);
	struct cbm_masks *cbm;
	char *dom = NULL, *id;
	struct resctrl_domain *d;
	unsigned long dom_id;

next:
	if (!line || line[0] == '\0')
		return 0;
	dom = strsep(&line, ";");
	id = strsep(&dom, "=");
	id = strim(id);
	if (!dom || kstrtoul(id, 10, &dom_id)) {
		// rdt_last_cmd_puts("Missing '=' or non-numeric domain\n");
		return -EINVAL;
	}
	dom = strim(dom);
	list_for_each_entry(d, &r->domains, list) {
		if (d->id != dom_id)
			continue;
		cbm = get_mydomain(d)->cbm_masks;
		if (!validate_mask(d, dom, cbm, closid))
			return -EINVAL;
		goto next;
	}
	return -EINVAL;
}

struct rdt_msr_info {
	int	msr_base;
	struct cbm_masks *cbm;
	struct mydomain *m;
};

static void update_msrs(void *info)
{
	struct rdt_msr_info *mi = info;

	for (int i = 0; i < cat.num_alloc_ids; i++) {
		if (mi->cbm[i].need_update) {
			if (state[i] == RESCTRL_EXCLUSIVE) {
				mi->m->exclusive_mask |= mi->cbm[i].staged & ~mi->cbm[i].now;
				mi->m->exclusive_mask &= ~(mi->cbm[i].now & ~mi->cbm[i].staged);
			}
			mi->cbm[i].now = mi->cbm[i].staged;
			mi->cbm[i].need_update = false;
			wrmsrl(mi->msr_base + i * MULDIV, mi->cbm[i].now);
		}
	}
}

static void applychanges(struct resctrl_resource *r, u64 resctrl_ids)
{
	int closid = (resctrl_ids >> 32);
	struct resctrl_domain *d;
	struct cbm_masks *cbm;
	struct rdt_msr_info mi;

	list_for_each_entry(d, &r->domains, list) {
		cbm = get_mydomain(d)->cbm_masks;
		if (!cbm[closid].need_update)
			continue;
		mi.msr_base = r->archtag;
		mi.cbm = cbm;
		mi.m = get_mydomain(d);
		smp_call_function_single(cpumask_first(&d->cpu_mask), update_msrs, &mi, 1);
	}
}

#ifdef CDP
static void update_cdp(void *info)
{
	u64 val;

	rdmsrl(MSRCDP, val);
	if (info)
		val |= BIT(0);
	else
		val &= ~BIT(0);
	wrmsrl(MSRCDP, val);
}
#endif

/*
 * On domain discovery (duing module load, or CPU hotplug) set
 * all controls to allow full access to all of cache. Ditto on
 * module unload or domain removal.
 */
static void domain_update(struct resctrl_resource *r, int what, int cpu, struct resctrl_domain *d)
{
	struct mydomain *m = get_mydomain(d);
	unsigned int eax, ebx, ecx, edx;
	struct rdt_msr_info mi;
	struct cbm_masks *cbm;

	cbm = (struct cbm_masks *)(m + 1);
	if (what == RESCTRL_DOMAIN_ADD || what == RESCTRL_DOMAIN_DELETE) {
		cpuid_count(0x10, LEAF_BIT, &eax, &ebx, &ecx, &edx);
		shareable_bits = ebx;
		m->cbm_len = eax & 0x1f;
		cbm_mask = (1u << (m->cbm_len + 1)) - 1;
		for (int i = 0; i < cat.num_alloc_ids; i++) {
			cbm[i].staged = cbm_mask;
			cbm[i].need_update = true;
		}
		mi.msr_base = r->archtag;
		mi.cbm = cbm;
		smp_call_function_single(cpu, update_msrs, &mi, 1);
	}
#ifdef CDP
	if (what == RESCTRL_DOMAIN_ADD)
		smp_call_function_single(cpu, update_cdp, (void *)1, 1);
	else if (what == RESCTRL_DOMAIN_DELETE)
		smp_call_function_single(cpu, update_cdp, NULL, 1);
#endif
}

static bool overlap_in_domain(struct mydomain *m, int closid)
{
	if (m->cbm_masks[closid].now & shareable_bits)
		return true;
	if (m->cbm_masks[closid].now & m->exclusive_mask)
		return true;
	for (int i = 0; i < num_closids; i++) {
		if (state[i] == RESCTRL_SHARED && i != closid)
			if (m->cbm_masks[closid].now & m->cbm_masks[i].now)
				return true;
	}

	return false;
}

static bool no_overlap(struct resctrl_resource *r, int closid)
{
	struct resctrl_domain *d;
	struct mydomain *m;

	list_for_each_entry(d, &r->domains, list) {
		m = get_mydomain(d);

		if (overlap_in_domain(m, closid))
			return false;
	}
	return true;
}

static bool setmode(struct resctrl_resource *r, u64 resctrl_ids, enum resctrl_mode mode)
{
	int closid = resctrl_ids >> 32;
	struct resctrl_domain *d;
	struct mydomain *m;
	struct rdt_msr_info mi;

	if (state[closid] == RESCTRL_FREE) {
		if (mode != RESCTRL_SHARED)
			return false;
		list_for_each_entry(d, &r->domains, list) {
			m = get_mydomain(d);
			m->cbm_masks[closid].staged = (1u << (m->cbm_len + 1)) - 1;
			m->cbm_masks[closid].staged &= ~m->exclusive_mask;
			if (m->cbm_masks[closid].staged != m->cbm_masks[closid].now) {
				m->cbm_masks[closid].need_update = true;
				mi.msr_base = r->archtag;
				mi.cbm = m->cbm_masks;
				smp_call_function_single(cpumask_first(&d->cpu_mask), update_msrs, &mi, 1);
			}
		}
	} else if (state[closid] == RESCTRL_SHARED && mode == RESCTRL_TRY_EXCLUSIVE) {
		return no_overlap(r, closid);
	} else if (state[closid] == RESCTRL_SHARED && mode == RESCTRL_EXCLUSIVE) {
		list_for_each_entry(d, &r->domains, list) {
			m = get_mydomain(d);
			m->exclusive_mask |= m->cbm_masks[closid].now;
		}
	} else if (state[closid] == RESCTRL_EXCLUSIVE) {
		list_for_each_entry(d, &r->domains, list) {
			m = get_mydomain(d);
			m->exclusive_mask &= ~m->cbm_masks[closid].now;
		}
	} else if (state[closid] != RESCTRL_SHARED) {
		return false;
	}

	state[closid] = mode;

	return true;
}

static void show_bits(struct seq_file *sf, struct mydomain *m)
{
	u64 group_shared_bits = 0ull;
	int i, idx;

	for (i = 0; i < num_closids; i++)
		if (state[i] == RESCTRL_SHARED)
			group_shared_bits |= m->cbm_masks[i].now;

	for (i = m->cbm_len; i >= 0; i--) {
		idx = 0;
		if (shareable_bits & BIT(i))
			idx |= BIT(2);
		if (group_shared_bits & BIT(i))
			idx |= BIT(1);
		if (m->exclusive_mask & BIT(i))
			idx |= BIT(0);
		seq_putc(sf, "0ES!H!X!"[idx]);
	}
}

static int bit_usage_show(struct seq_file *sf, void *v)
{
	struct kernfs_open_file *of = sf->private;
	struct resctrl_resource *r;
	struct resctrl_domain *d;
	struct mydomain *m;
	bool sep = false;

	r = of->kn->priv;
	list_for_each_entry(d, &r->domains, list) {
		m = get_mydomain(d);
		if (sep)
			seq_puts(sf, ";");
		seq_printf(sf, "%d=", d->id);
		show_bits(sf, m);
		sep = true;
	}
	seq_puts(sf, "\n");

	return 0;
}

static struct kernfs_ops bit_usage_ops = {
	.seq_show = bit_usage_show,
};

RESCTRL_FILE_DEF(cbm_mask, "%x\n")
RESCTRL_FILE_DEF(min_cbm_bits, "%d\n")
RESCTRL_FILE_DEF(num_closids, "%d\n")
RESCTRL_FILE_DEF(shareable_bits, "%x\n")

static struct resctrl_fileinfo cat_files[] = {
	{ .name = "cbm_mask", .ops = &cbm_mask_ops },
	{ .name = "min_cbm_bits", .ops = &min_cbm_bits_ops },
	{ .name = "num_closids", .ops = &num_closids_ops },
	{ .name = "shareable_bits", .ops = &shareable_bits_ops },
	{ .name = "bit_usage", .ops = &bit_usage_ops },
	{ }
};

static struct resctrl_resource cat = {
	.name		= NAME SUFFIX_D,
	.archtag	= MSR,
	.type		= RESCTRL_CONTROL,
	.show		= show,
	.resetstaging	= resetstaging,
	.parse		= parse,
	.applychanges	= applychanges,
	.scope		= SCOPE,
	.domain_size	= sizeof(struct resctrl_domain) + sizeof(struct mydomain),
	.domains	= LIST_HEAD_INIT(cat.domains),
	.domain_update	= domain_update,
	.setmode	= setmode,
	.infodir	= "L3" SUFFIX_D,
	.infofiles	= cat_files,
};

#ifdef CDP
static struct resctrl_resource cat_code = {
	.name		= NAME SUFFIX_C,
	.archtag	= MSR + 1,
	.type		= RESCTRL_CONTROL,
	.show		= show,
	.resetstaging	= resetstaging,
	.parse		= parse,
	.applychanges	= applychanges,
	.scope		= SCOPE,
	.domain_size	= sizeof(struct resctrl_domain) + sizeof(struct mydomain),
	.domains	= LIST_HEAD_INIT(cat_code.domains),
	.domain_update	= domain_update,
	.setmode	= setmode,
	.infodir	= "L3" SUFFIX_C,
	.infofiles	= cat_files,
};
#endif

static int __init cat_init(void)
{
	unsigned int eax, ebx, ecx, edx, cat_features;
	int ret;

	if (!boot_cpu_has(X86_FEATURE_RDT_A)) {
		pr_debug("No RDT allocation support\n");
		return -ENODEV;
	}

	cat_features = cpuid_ebx(0x10);

	if (!(cat_features & BIT(LEAF_BIT))) {
		pr_debug("No RDT allocation for L%d cache\n", CACHE_LEVEL);
		return -ENODEV;
	}

	cpuid_count(0x10, LEAF_BIT, &eax, &ebx, &ecx, &edx);
#ifdef CDP
	if (!(ecx & BIT(2))) {
		pr_debug("No CDP mode for L%d cache\n", CACHE_LEVEL);
		return -ENODEV;
	}
#endif
	num_closids = (edx + 1) / MULDIV;
	state = kcalloc(num_closids, sizeof(*state), GFP_KERNEL);

	cat.domain_size += num_closids * sizeof(struct cbm_masks);
	cat.num_alloc_ids = num_closids;
#ifdef CDP
	cat_code.domain_size += num_closids * sizeof(struct cbm_masks);
	cat_code.num_alloc_ids = num_closids;
#endif

	if (boot_cpu_data.x86_vendor == X86_VENDOR_AMD) {
		min_cbm_bits = 0;
		arch_has_sparse_bitmaps = true;
	}

	ret = resctrl_register_ctrl_resource(&cat);
#ifdef CDP
	if (!ret)
		ret = resctrl_register_ctrl_resource(&cat_code);
	if (ret)
		resctrl_unregister_ctrl_resource(&cat);
#endif
	return ret;
}

static void __exit cat_cleanup(void)
{
	resctrl_unregister_ctrl_resource(&cat);
#ifdef CDP
	resctrl_unregister_ctrl_resource(&cat_code);
#endif
}

module_init(cat_init);
module_exit(cat_cleanup);

MODULE_LICENSE("GPL");
