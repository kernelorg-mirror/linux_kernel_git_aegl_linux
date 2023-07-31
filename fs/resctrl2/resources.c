// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2023 Intel Corporation. */

#include "internal.h"

LIST_HEAD(resctrl_all_resources);
int resctrl_pseudolock_available;

void resctrl_ctrl_callback(void (*fn)(u64 resctrl_ids, void *v), void *v)
{
	struct resctrl_group *rg;

	list_for_each_entry(rg, &all_ctrl_groups, list)
		fn(rg->resctrl_ids, v);
}
EXPORT_SYMBOL_GPL(resctrl_ctrl_callback);

int resctrl_activate(struct resctrl_resource *r)
{
	if (r->infodir)
		resctrl_addinfofiles(r);

	if (r->type == RESCTRL_MONITOR)
		arch_add_monitor(r->mon_event);

	return 0;
}

int resctrl_register_resource(struct resctrl_resource *r)
{
	struct resctrl_resource *t;
	struct resctrl_group *rg;
	int ret = 0;
	int cpu;

	cpus_read_lock();
	mutex_lock(&resctrl_mutex);

	if (r->type == RESCTRL_CONTROL) {
		for_each_resource(t) {
			if (r->archtag == t->archtag) {
				ret = -EEXIST;
				goto out;
			}
		}

		if (r->setmode) {
			list_for_each_entry(rg, &all_ctrl_groups, list) {
				if (rg->mode == RESCTRL_EXCLUSIVE) {
					ret = -EINVAL;
					goto out;
				}

				r->setmode(r, rg->resctrl_ids, rg->mode);
			}
		}

		if (r->num_alloc_ids) {
			if (!arch_init_alloc_ids(r)) {
				ret = -ENOSPC;
				goto out;
			}
		}
	}

	if (r->domain_size)
		for_each_online_cpu(cpu)
			resctrl_domain_add_cpu(cpu, r);

	if (r->pseudo_lock_parse)
		resctrl_pseudolock_available++;

	if (resctrl_is_mounted)
		ret = resctrl_activate(r);

	list_add(&r->list, &resctrl_all_resources);
out:
	mutex_unlock(&resctrl_mutex);
	cpus_read_unlock();

	return ret;
}
EXPORT_SYMBOL_GPL(resctrl_register_resource);

void resctrl_deactivate(struct resctrl_resource *r, struct list_head *h)
{
	if (r->reset)
		r->reset(r);

	if (r->type == RESCTRL_MONITOR)
		arch_del_monitor(r->mon_event);

	if (r->infodir)
		resctrl_delinfofiles(r, h);
}

void resctrl_unregister_resource(struct resctrl_resource *r)
{
	struct resctrl_group *rg, *tmp;
	struct resctrl_node_info *rni;
	int cpu;

	LIST_HEAD(clean_list);

	cpus_read_lock();
	mutex_lock(&resctrl_mutex);

	if (resctrl_is_mounted) {
		if (r->pseudo_lock_parse) {
			list_for_each_entry_safe(rg, tmp, &all_ctrl_groups, list) {
				if (rg->mode != RESCTRL_PSEUDO_LOCKED || rg->plr.r != r)
					continue;
				rni = (struct resctrl_node_info *)rg - 1;
				list_del(&rg->list);
				atomic_inc(&rni->waitcount);
				rni->flags |= RESCTRL_DELETED;
				kernfs_remove(rni->kn);
				list_add(&rni->clean_list, &clean_list);
			}
			resctrl_pseudolock_available--;
		}
		resctrl_deactivate(r, &clean_list);
	}

	if (r->domain_size)
		for_each_online_cpu(cpu)
			resctrl_domain_remove_cpu(cpu, r, &clean_list);

	list_del(&r->list);
	mutex_unlock(&resctrl_mutex);
	cpus_read_unlock();

	resctrl_node_file_cleanup(&clean_list);
}
EXPORT_SYMBOL_GPL(resctrl_unregister_resource);

struct resctrl_plr *resctrl_find_group_by_minor(unsigned int minor)
{
	struct resctrl_plr *plr = NULL;
	struct resctrl_node_info *rni;
	struct resctrl_group *rg;

	mutex_lock(&resctrl_mutex);
	list_for_each_entry(rg, &all_ctrl_groups, list) {
		if (rg->plr.minor == minor) {
			rni = (struct resctrl_node_info *)rg - 1;
			if (!(rni->flags & RESCTRL_DELETED)) {
				plr = &rg->plr;
				resctrl_kn_get(rni, rni->kn);
			}
			break;
		}
	}
	mutex_unlock(&resctrl_mutex);

	return plr;
}
EXPORT_SYMBOL_GPL(resctrl_find_group_by_minor);

void resctrl_release_group_by_plr(struct resctrl_plr *plr)
{
	struct resctrl_node_info *rni = plr->private;

	resctrl_kn_put(rni, rni->kn);
}
EXPORT_SYMBOL_GPL(resctrl_release_group_by_plr);
