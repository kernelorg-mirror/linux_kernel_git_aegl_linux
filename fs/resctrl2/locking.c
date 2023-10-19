// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2023 Intel Corporation. */

#include "internal.h"

/* Mutex to protect resctrl group access. */
DEFINE_MUTEX(resctrl_mutex);

void resctrl_kn_put(struct resctrl_node_info *rni, struct kernfs_node *kn)
{
	if (atomic_dec_and_test(&rni->waitcount) &&
	    (rni->flags & RESCTRL_DELETED)) {
		kernfs_unbreak_active_protection(kn);
		resctrl_node_remove(rni);
	} else {
		kernfs_unbreak_active_protection(kn);
	}
}
