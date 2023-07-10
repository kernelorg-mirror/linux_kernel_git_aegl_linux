// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2023 Intel Corporation. */

#include "internal.h"

/* Mutex to protect resctrl group access. */
DEFINE_MUTEX(resctrl_mutex);

void resctrl_kn_get(struct resctrl_node_info *rni, struct kernfs_node *kn)
{
	atomic_inc(&rni->waitcount);
	kernfs_break_active_protection(kn);
}

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

struct resctrl_node_info *resctrl_kn_lock_live(struct kernfs_node *kn)
{
	struct resctrl_node_info *rni = kn->priv;

	WARN_ON(!rni);

	if (rni->flags & RESCTRL_LOCK_CPUS)
		cpus_read_lock();

	resctrl_kn_get(rni, kn);
	mutex_lock(&resctrl_mutex);

	if (rni->flags & RESCTRL_DELETED)
		return NULL;

	return rni;
}

void resctrl_kn_unlock(struct kernfs_node *kn)
{
	struct resctrl_node_info *rni = kn->priv;

	if (!rni)
		return;

	mutex_unlock(&resctrl_mutex);
	resctrl_kn_put(rni, kn);

	if (rni->flags & RESCTRL_LOCK_CPUS)
		cpus_read_unlock();

}
