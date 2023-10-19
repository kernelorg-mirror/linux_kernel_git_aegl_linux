/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright(c) 2023 Intel Corporation. */

#include <linux/cpu.h>
#include <linux/kernfs.h>
#include <linux/resctrl.h>
#include <linux/seq_buf.h>

enum directory_type {
	DIR_INFO,
};

struct resctrl_group {
	enum directory_type	type;
};

#define for_each_resource(r)					\
	list_for_each_entry(r, &resctrl_all_resources, list)

struct resctrl_node_info {
	int			type;
	struct kernfs_node	*kn;
};

#define RESCTRL_INFOFILE	2

// info.c
bool resctrl_add_info_dir(struct kernfs_node *parent_kn);
void resctrl_addinfofiles(struct resctrl_resource *r);
void resctrl_delinfofiles(struct resctrl_resource *r);

// kernfs.c
struct kernfs_node *resctrl_add_dir(struct kernfs_node *parent_kn, const char *name,
				    void *priv);
struct resctrl_node_info *resctrl_add_file(struct kernfs_node *parent_kn, char *name,
					   umode_t mode, int type);

// locking.c
extern struct mutex resctrl_mutex;

// resources.c
extern struct list_head resctrl_all_resources;
void resctrl_activate(struct resctrl_resource *r);
void resctrl_deactivate(struct resctrl_resource *r);

// root.c
extern bool resctrl_is_mounted;
