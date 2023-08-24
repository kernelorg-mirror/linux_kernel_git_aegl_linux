// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2023 Intel Corporation. */

/*
 * AMD -  Bandwidth Monitoring Event Configuration.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/resctrl.h>
#include <linux/seq_file.h>

#include "rdt.h"

#undef pr_fmt
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#define NUM_MBEC_FILES	2
#define TOTAL_DEFAULT	0x7f
#define LOCAL_DEFAULT	0x15
#define MAX_EVENT_MASK	0x7f

struct masks {
	u32	now;
	u32	staged;
	bool	need_update;
};

struct mydomain {
	RESCTRL_DOMAIN_HEADER;
	struct masks total;
	struct masks local;
};

static struct resctrl_resource mbec;
static struct resctrl_fileinfo mbec_files[NUM_MBEC_FILES + 1];

static int do_show(struct seq_file *sf, struct resctrl_resource *r, int which)
{
	struct mydomain *m;
	char *sep = "";
	u32 val;

	list_for_each_entry(m, &r->domains, list) {
		val = which ? m->total.now : m->local.now;
		seq_printf(sf, "%s%d=0x%x", sep, m->id, val);
		sep = ";";
	}
	seq_printf(sf, "\n");

	return 0;
}
static int total_show(struct seq_file *sf, struct resctrl_resource *r)
{
	return do_show(sf, r, 1);
}

static int local_show(struct seq_file *sf, struct resctrl_resource *r)
{
	return do_show(sf, r, 0);
}

static void update_msrs(void *info)
{
	struct mydomain *m = info;

	if (m->total.need_update) {
		m->total.now = m->total.staged;
		m->total.need_update = false;
		wrmsrl(MSR_IA32_EVT_CFG_BASE, m->total.now);
	}
	if (m->local.need_update) {
		m->local.now = m->local.staged;
		m->local.need_update = false;
		wrmsrl(MSR_IA32_EVT_CFG_BASE + 1, m->local.now);
	}
}

static ssize_t do_write(char *buf, size_t nbytes, int which)
{
	char *dom = NULL, *id;
	unsigned long dom_id;
	struct mydomain *m;
	unsigned long event_mask;

	resctrl_last_cmd_clear();
next:
	if (!buf || buf[0] == '\0')
		goto update;

	dom = strsep(&buf, ";");
	id = strsep(&dom, "=");
	id = strim(id);
	if (!dom || kstrtoul(id, 10, &dom_id)) {
		resctrl_last_cmd_puts("Missing '=' or non-numeric domain\n");
		return -EINVAL;
	}
	dom = strim(dom);
	list_for_each_entry(m, &mbec.domains, list) {
		if (m->id != dom_id)
			continue;
		if (kstrtoul(dom, 16, &event_mask) || event_mask == 0 ||
		    event_mask > MAX_EVENT_MASK) {
			resctrl_last_cmd_puts("Bad event mask\n");
			return -EINVAL;
		}
		switch (which) {
		case 0:
			if (event_mask == m->local.now)
				goto next;
			m->local.staged = event_mask;
			m->local.need_update = true;
			break;
		case 1:
			if (event_mask == m->total.now)
				goto next;
			m->total.staged = event_mask;
			m->total.need_update = true;
			break;
		}
		goto next;
	}
	resctrl_last_cmd_printf("unknown domain %ld\n", dom_id);
	return -EINVAL;

update:
	list_for_each_entry(m, &mbec.domains, list) {
		if (m->local.need_update || m->total.need_update)
			smp_call_function_single(cpumask_first(&m->cpu_mask), update_msrs, m, 1);
	}

	return nbytes;
}

static ssize_t total_write(char *buf, size_t nbytes)
{
	return do_write(buf, nbytes, 1);
}

static ssize_t local_write(char *buf, size_t nbytes)
{
	return do_write(buf, nbytes, 0);
}

static void domain_update(struct resctrl_resource *r, int what, int cpu, void *domain)
{
	struct mydomain *m = domain;

	if (what == RESCTRL_DOMAIN_ADD || what == RESCTRL_DOMAIN_DELETE) {
		m->total.staged = TOTAL_DEFAULT;
		m->total.need_update = true;
		m->local.staged = LOCAL_DEFAULT;
		m->local.need_update = true;
	}
	smp_call_function_single(cpumask_first(&m->cpu_mask), update_msrs, m, 1);
}

static void reset(struct resctrl_resource *r)
{
	struct mydomain *m;

	list_for_each_entry(m, &r->domains, list) {
		if (m->total.now != TOTAL_DEFAULT) {
			m->total.staged = TOTAL_DEFAULT;
			m->total.need_update = true;
		}
		if (m->local.now != LOCAL_DEFAULT) {
			m->local.staged = LOCAL_DEFAULT;
			m->local.need_update = true;
		}
		if (m->total.need_update || m->local.need_update)
			smp_call_function_single(cpumask_first(&m->cpu_mask), update_msrs, m, 1);
	}
}

static struct resctrl_resource mbec = {
	.scope		= RESCTRL_L3CACHE,
	.domains	= LIST_HEAD_INIT(mbec.domains),
	.domain_size	= sizeof(struct mydomain),
	.domain_update	= domain_update,
	.infodir	= "L3_MON",
	.infofiles	= mbec_files,
	.reset		= reset,
};

static int __init mbec_init(void)
{
	int	file = 0;

	if (boot_cpu_data.x86_vendor != X86_VENDOR_AMD ||
	    !boot_cpu_has(X86_FEATURE_BMEC))
		return -ENODEV;

	if (boot_cpu_has(X86_FEATURE_CQM_MBM_TOTAL)) {
		mbec_files[file].name = "mbm_total_bytes_config";
		mbec_files[file].show = total_show;
		mbec_files[file].write = total_write;
		file++;
	}
	if (boot_cpu_has(X86_FEATURE_CQM_MBM_LOCAL)) {
		mbec_files[file].name = "mbm_local_bytes_config";
		mbec_files[file].show = local_show;
		mbec_files[file].write = local_write;
		file++;
	}

	if (!file)
		return -ENODEV;

	return resctrl_register_resource(&mbec);
}

static void __exit mbec_cleanup(void)
{
	resctrl_unregister_resource(&mbec);
}

module_init(mbec_init);
module_exit(mbec_cleanup);

MODULE_AUTHOR("Tony Luck <tony.luck@intel.com>");
MODULE_IMPORT_NS(RESCTRL);
MODULE_LICENSE("GPL");
