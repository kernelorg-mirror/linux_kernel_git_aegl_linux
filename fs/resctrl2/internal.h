/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright(c) 2023 Intel Corporation. */

#include <linux/cpu.h>
#include <linux/kernfs.h>

struct resctrl_node_info {
	struct kernfs_node	*kn;
};

// locking.c
extern struct mutex resctrl_mutex;
