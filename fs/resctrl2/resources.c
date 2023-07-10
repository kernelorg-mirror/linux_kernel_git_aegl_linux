// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2023 Intel Corporation. */

#include "internal.h"

LIST_HEAD(resctrl_all_resources);

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

	if (r->type == RESCTRL_MONITOR && r->mon_domain_file)
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

	if (r->type == RESCTRL_MONITOR && r->mon_domain_file)
		arch_del_monitor(r->mon_event);

	if (r->infodir)
		resctrl_delinfofiles(r, h);
}

void resctrl_unregister_resource(struct resctrl_resource *r)
{
	int cpu;

	LIST_HEAD(mon_file_clean_list);

	cpus_read_lock();
	mutex_lock(&resctrl_mutex);

	if (resctrl_is_mounted)
		resctrl_deactivate(r, &mon_file_clean_list);
	if (r->domain_size)
		for_each_online_cpu(cpu)
			resctrl_domain_remove_cpu(cpu, r, &mon_file_clean_list);
	list_del(&r->list);
	mutex_unlock(&resctrl_mutex);
	cpus_read_unlock();

	resctrl_node_file_cleanup(&mon_file_clean_list);
}
EXPORT_SYMBOL_GPL(resctrl_unregister_resource);
