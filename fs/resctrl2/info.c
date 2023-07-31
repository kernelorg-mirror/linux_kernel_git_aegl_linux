// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2023 Intel Corporation. */

#include "internal.h"

static struct kernfs_node *kn_info;

static struct resctrl_group info_header = {
	.type = DIR_INFO
};

static struct seq_buf last_cmd_status;
static char last_cmd_status_buf[512];

void resctrl_last_cmd_clear(void)
{
	seq_buf_clear(&last_cmd_status);
}

void resctrl_last_cmd_puts(const char *s)
{
	seq_buf_puts(&last_cmd_status, s);
}
EXPORT_SYMBOL_GPL(resctrl_last_cmd_puts);

void resctrl_last_cmd_printf(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	seq_buf_vprintf(&last_cmd_status, fmt, ap);
	va_end(ap);
}
EXPORT_SYMBOL_GPL(resctrl_last_cmd_printf);

static int last_cmd_status_seq_show(struct seq_file *m, void *arg)
{
	int len;

	len = seq_buf_used(&last_cmd_status);
	if (len)
		seq_printf(m, "%.*s", len, last_cmd_status_buf);
	else
		seq_puts(m, "ok\n");

	return 0;
}

static struct kernfs_ops cmd_status_ops = {
	.seq_show = last_cmd_status_seq_show,
};

bool resctrl_add_info_dir(struct kernfs_node *parent_kn)
{
	struct kernfs_node *kn;

	seq_buf_init(&last_cmd_status, last_cmd_status_buf,
		     sizeof(last_cmd_status_buf));

	kn_info = resctrl_add_dir(parent_kn, "info", &info_header);
	if (!kn_info)
		return false;

	kn = __resctrl_add_file(kn_info, "last_cmd_status", 0444, &cmd_status_ops, NULL);
	if (!kn) {
		kernfs_remove(kn_info);
		return false;
	}

	return true;
}

void resctrl_addinfofiles(struct resctrl_resource *r)
{
	struct resctrl_node_info *rni;
	struct kernfs_node *pkn;
	struct resctrl_fileinfo *f;
	struct info_file_info *ifi;
	umode_t mode;

	pkn = resctrl_add_dir(kn_info, r->infodir, NULL);
	if (!pkn)
		return;

	for (f = r->infofiles; f->name; f++) {
		mode = (f->write) ? 0644 : 0444;
		rni = resctrl_add_file(pkn, f->name, mode, RESCTRL_INFOFILE);
		if (!rni)
			break;
		ifi = (struct info_file_info *)&rni->priv;
		ifi->r = r;
		ifi->show = f->show;
		ifi->write = f->write;
	}
	kernfs_activate(pkn);
}

void resctrl_delinfofiles(struct resctrl_resource *r, struct list_head *h)
{
	struct kernfs_node *pkn;
	struct resctrl_fileinfo *f;

	pkn = kernfs_find_and_get_ns(kn_info, r->infodir, NULL);
	if (!pkn)
		return;

	for (f = r->infofiles; f->name; f++)
		resctrl_remove_file(f->name, pkn, h);
	kernfs_remove(pkn);
}
