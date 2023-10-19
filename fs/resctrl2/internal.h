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

struct resctrl_node_info {
	struct kernfs_node	*kn;
};

// info.c
bool resctrl_add_info_dir(struct kernfs_node *parent_kn);

// kernfs.c
struct kernfs_node *resctrl_add_dir(struct kernfs_node *parent_kn, const char *name,
				    void *priv);

// locking.c
extern struct mutex resctrl_mutex;
