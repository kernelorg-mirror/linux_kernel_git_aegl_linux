// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2023 Intel Corporation. */

/*
 *  IO RDT driver
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/acpi.h>
#include <linux/mod_devicetable.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/resctrl.h>

#include <asm/cpu_device_id.h>

#undef pr_fmt
#define pr_fmt(fmt) "iordt: " fmt

struct rdt_rcs {
	void __iomem *closbase;
	void __iomem *rmidbase;
	int reg_width;
	int rmud, rcs;
	int channel_base;
};

struct rdt_channel {
	void __iomem *mmioclos;
	void __iomem *mmiormid;
	u64	ids;
	char	*devices;
	int	reg_width;
	int	valid;
};

static int num_rcs;
static int num_channels;
static struct rdt_rcs *rdt_rcs;
static struct rdt_channel *rdt_channels;
static bool iordt_mon, iordt_cat;

/* common header for acpi_table_rcs and acpi_table_dss */
struct rcs_dss_hdr {
	u16	type;
	u16	length;
};

enum rcs_dss_type {
	DSS_TYPE,
	RCS_TYPE
};

static void mmio_writew(int val, void __iomem *addr)
{
	pr_debug("writew(val=0x%x, addr=0x%lx)\n", val, (long)addr);
	*(u16 *)addr = val;
}

static void mmio_writeq(u64 val, void __iomem *addr)
{
	pr_debug("writeq(val=0x%llx, addr=0x%lx)\n", val, (long)addr);
	*(u64 *)addr = val;
}

static void change_channel(struct rdt_channel *c, u64 resctrl_ids)
{
	int closid = resctrl_ids >> 32;
	int rmid = resctrl_ids & 0xffff;

	c->ids = resctrl_ids;

	if (c->reg_width == 2) {
		if (iordt_cat)
			mmio_writew(closid, c->mmioclos);
		if (iordt_mon)
			mmio_writew(rmid, c->mmiormid);
	} else {
		if (iordt_cat)
			mmio_writeq(closid, c->mmioclos);
		if (iordt_mon)
			mmio_writeq(rmid, c->mmiormid);
	}
}

static void walk_rmuds(struct acpi_table_irdt *h, int type,
		       void (*call)(struct rcs_dss_hdr *rdh, int seg, int r, int i))
{
	struct acpi_table_rmud *rmud = (void *)h + sizeof(*h);
	struct acpi_table_rmud *irdtend = (void *)h + h->header.length;
	int i = 0, r = 0;

	while (rmud < irdtend) {
		if (rmud->type == 0) {
			struct rcs_dss_hdr *rcs_dss, *end;

			rcs_dss = (void *)rmud + sizeof(*rmud);
			end = (void *)rmud + rmud->length;

			while (rcs_dss < end) {
				if (rcs_dss->type == type)
					call(rcs_dss, rmud->segment, r, i++);
				rcs_dss = (void *)rcs_dss + rcs_dss->length;
			}
		}
		rmud = (void *)rmud + rmud->length;
		r++;
	}
}

static void count_rcs(struct rcs_dss_hdr *rdh, int seg, int rmud, int idx)
{
	num_rcs++;
}

static void get_rcs_info(struct rcs_dss_hdr *rdh, int seg, int rmud, int idx)
{
	struct acpi_table_rcs *rcs = (struct acpi_table_rcs *)rdh;
	unsigned long addr;
	int size;

	rdt_rcs[idx].reg_width = (rcs->flags & BIT(3)) ? 2 : 8;
	size = rdt_rcs[idx].reg_width * rcs->channel_count;

	addr = rcs->block_mmio_location + rcs->clos_block_offset;
	rdt_rcs[idx].closbase = ioremap(addr, size);

	addr = rcs->block_mmio_location + rcs->rmid_block_offset;
	rdt_rcs[idx].rmidbase = ioremap(addr, size);

	rdt_rcs[idx].rcs = rcs->id;
	rdt_rcs[idx].rmud = rmud;
	rdt_rcs[idx].channel_base = num_channels;
	num_channels += rcs->channel_count;
}

static struct rdt_rcs *lookup_rcs(int rmud, int rcs)
{
	int i;

	for (i = 0; i < num_rcs; i++)
		if (rdt_rcs[i].rmud == rmud && rdt_rcs[i].rcs == rcs)
			return &rdt_rcs[i];
	return NULL;
}

static void update_channel(struct rdt_rcs *r, int chan, int seg, int bdf, int vc)
{
	struct rdt_channel *c = &rdt_channels[r->channel_base + chan];

	if (!c->valid) {
		c->mmioclos = (void __iomem *)(r->closbase + chan * r->reg_width);
		c->mmiormid = (void __iomem *)(r->rmidbase + chan * r->reg_width);
		c->reg_width = r->reg_width;
		c->devices = kasprintf(GFP_KERNEL, "%.4x:%.2x:%.2x.%x.VC%x\n", seg,
				       bdf >> 8, (bdf >> 3) & 0x1f, bdf & 0x7, vc);
		c->valid = 1;
	} else {
		char *tmp;

		tmp = kasprintf(GFP_KERNEL, "%s%.4x:%.2x:%.2x.%x.VC%x\n",
				c->devices, seg,
				bdf >> 8, (bdf >> 3) & 0x1f, bdf & 0x7, vc);
		kfree(c->devices);
		c->devices = tmp;
	}
}

static void get_dss_info(struct rcs_dss_hdr *rdh, int seg, int rmud, int idx)
{
	struct acpi_table_dss *dss = (struct acpi_table_dss *)rdh;
	unsigned char *p = (unsigned char *)dss + sizeof(*dss);
	struct rdt_rcs *rrcs;
	int i;

	while (p < (unsigned char *)dss + rdh->length) {
		rrcs = lookup_rcs(rmud, p[0]);

		for (i = 1; i < 8; i++) {
			if (!(p[i] & BIT(7)))
				continue;
			update_channel(rrcs, p[i] & 0x3f, seg, dss->id, i - 1);
		}
		p += 16;
	}
}

static int info_chan_show(struct seq_file *sf)
{
	struct kernfs_open_file *kf = sf->private;
	int chan;

	if (kstrtoint(kf->kn->name, 10, &chan) || chan < 0 ||
	    chan >= num_channels || !rdt_channels[chan].valid)
		return -EINVAL;

	seq_puts(sf, rdt_channels[chan].devices);

	return 0;
}

static ssize_t ctrl_chan_write(char *buf, size_t nbytes, resctrl_ids_t resctrl_ids)
{
	int chan;

	if (kstrtoint(strstrip(buf), 0, &chan) || chan < 0 ||
	    chan >= num_channels || !rdt_channels[chan].valid)
		return -EINVAL;

	change_channel(&rdt_channels[chan], resctrl_ids);

	return nbytes;
}

static int ctrl_chan_show(struct seq_file *sf, resctrl_ids_t resctrl_ids)
{
	for (int i = 0; i < num_channels; i++) {
		if (!rdt_channels[i].valid)
			continue;
		if (rdt_channels[i].ids == resctrl_ids)
			seq_printf(sf, "%d\n", i);
	}

	return 0;
}

static void rmdir(resctrl_ids_t old_ids, resctrl_ids_t new_ids)
{
	for (int i = 0; i < num_channels; i++) {
		if (!rdt_channels[i].valid)
			continue;
		if (rdt_channels[i].ids == old_ids)
			change_channel(&rdt_channels[i], new_ids);
	}
}

#define MSR_IA32_L3_IO_QOS_CFG	0xc83

static void update_msr(void *info)
{
	u64 val = 0;

	rdmsrl(MSR_IA32_L3_IO_QOS_CFG, val);
	if (info)
		val |= BIT_ULL(0);
	else
		val &= ~BIT_ULL(0);
	wrmsrl(MSR_IA32_L3_IO_QOS_CFG, val);
}

static void domain_update(struct resctrl_resource *r, int what, int cpu, void *domain)
{
	/* first domain coming online, or last domain going offline */
	if (list_empty(&r->domains)) {
		for (int i = 0; i < num_channels; i++) {
			if (!rdt_channels[i].valid)
				continue;
			change_channel(&rdt_channels[i], 0);
		}
	}

	if (what == RESCTRL_DOMAIN_ADD)
		smp_call_function_single(cpu, update_msr, (void *)1, 1);
	else if (what == RESCTRL_DOMAIN_DELETE)
		smp_call_function_single(cpu, update_msr, NULL, 1);
}

static struct resctrl_ctrlfileinfo files[] = {
	{
		.name	= "channels",
		.flags	= RESCTRL_CTRLMON_FILE | RESCTRL_MON_FILE,
		.show	= ctrl_chan_show,
		.write	= ctrl_chan_write,
	},
	{ }
};

static struct resctrl_resource iordt = {
	.name		= "IO",
	.scope		= RESCTRL_SOCKET,
	.domain_size	= sizeof(struct resctrl_domain),
	.domains	= LIST_HEAD_INIT(iordt.domains),
	.domain_update	= domain_update,
	.infodir	= "IO",
	.ctrlfiles	= files,
	.rmdir		= rmdir,
};

static void __init acpi_parse_irdt(struct acpi_table_irdt *irdt)
{
	struct resctrl_fileinfo *files;
	int n = 0;

	walk_rmuds(irdt, RCS_TYPE, count_rcs);
	rdt_rcs = kmalloc_array(num_rcs, sizeof(*rdt_rcs), GFP_KERNEL | __GFP_ZERO);

	walk_rmuds(irdt, RCS_TYPE, get_rcs_info);

	rdt_channels = kmalloc_array(num_channels, sizeof(*rdt_channels), GFP_KERNEL | __GFP_ZERO);

	walk_rmuds(irdt, DSS_TYPE, get_dss_info);

	for (int i = 0; i < num_channels; i++)
		if (rdt_channels[i].valid)
			n++;

	files = kmalloc_array(n + 1, sizeof(*files), GFP_KERNEL);

	n = 0;
	for (int i = 0; i < num_channels; i++) {
		if (!rdt_channels[i].valid)
			continue;
		files[n].name = kasprintf(GFP_KERNEL, "%d", i);
		files[n].show = info_chan_show;
		n++;
	}
	files[n].name = NULL;

	iordt.infofiles = files;
}

static const struct x86_cpu_id iordt_features[] = {
	X86_MATCH_FEATURE(X86_FEATURE_CAT_L3_IO, 0),
	X86_MATCH_FEATURE(X86_FEATURE_CQM_OCCUP_LLC_IO, 0),
	X86_MATCH_FEATURE(X86_FEATURE_CQM_MBM_IO, 0),
	{ }
};
MODULE_DEVICE_TABLE(x86cpu, iordt_features);

static int __init init_iordt(void)
{
	struct acpi_table_header *tbl;
	acpi_status status;

	if (!x86_match_cpu(iordt_features))
		return -ENODEV;

	iordt_cat = boot_cpu_has(X86_FEATURE_CAT_L3_IO);
	iordt_mon = boot_cpu_has(X86_FEATURE_CQM_OCCUP_LLC_IO) ||
		    boot_cpu_has(X86_FEATURE_CQM_MBM_IO);

	if (!iordt_cat && !iordt_mon)
		return -EINVAL;

	status = acpi_get_table(ACPI_SIG_IRDT, 0, &tbl);
	if (ACPI_FAILURE(status))
		return -EINVAL;

	acpi_parse_irdt((struct acpi_table_irdt *)tbl);

	resctrl_register_resource(&iordt);

	return 0;
}

static void __exit cleanup_iordt(void)
{
	int i;

	resctrl_unregister_resource(&iordt);
	for (i = 0; i < num_rcs; i++) {
		iounmap(rdt_rcs[i].closbase);
		iounmap(rdt_rcs[i].rmidbase);
	}
	kfree(rdt_rcs);

	for (i = 0; i < num_channels; i++)
		if (rdt_channels[i].valid)
			kfree(rdt_channels[i].devices);
	kfree(rdt_channels);

	for (i = 0; iordt.infofiles[i].name; i++)
		kfree(iordt.infofiles[i].name);
	kfree(iordt.infofiles);
}

module_init(init_iordt);
module_exit(cleanup_iordt);

MODULE_LICENSE("GPL");
MODULE_IMPORT_NS(RESCTRL);
