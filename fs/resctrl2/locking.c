// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2023 Intel Corporation. */

#include "internal.h"

/* Mutex to protect resctrl group access. */
DEFINE_MUTEX(resctrl_mutex);

struct resctrl_group *kernfs_to_resctrl_group(struct kernfs_node *kn)
{
	if (kernfs_type(kn) == KERNFS_DIR)
		return kn->priv;
	else
		return kn->parent->priv;
}

struct resctrl_group *resctrl_group_kn_lock_live(struct kernfs_node *kn)
{
	struct resctrl_group *rg = kernfs_to_resctrl_group(kn);

	if (!rg)
		return NULL;

	atomic_inc(&rg->waitcount);
	kernfs_break_active_protection(kn);

	mutex_lock(&resctrl_mutex);

	/* Was this group deleted while we waited? */
	if (rg->flags & RESCTRL_DELETED)
		return NULL;

	return rg;
}

void resctrl_group_kn_unlock(struct kernfs_node *kn)
{
	struct resctrl_group *rg = kernfs_to_resctrl_group(kn);

	if (!rg)
		return;

	mutex_unlock(&resctrl_mutex);

	if (atomic_dec_and_test(&rg->waitcount) &&
	    (rg->flags & RESCTRL_DELETED)) {
		kernfs_unbreak_active_protection(kn);
		resctrl_group_remove(rg);
	} else {
		kernfs_unbreak_active_protection(kn);
	}
}
