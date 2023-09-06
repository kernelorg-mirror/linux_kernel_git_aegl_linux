// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2023 Intel Corporation. */

#include "internal.h"

static struct kernfs_node *kn_info;

static struct resctrl_group info_header = {
	.type = DIR_INFO
};

bool resctrl_add_info_dir(struct kernfs_node *parent_kn)
{
	kn_info = resctrl_add_dir(parent_kn, "info", &info_header);
	if (!kn_info)
		return false;

	return true;
}
