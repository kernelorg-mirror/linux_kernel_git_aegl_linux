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

int resctrl_register_ctrl_resource(struct resctrl_resource *r)
{
	struct resctrl_resource *t;
	struct resctrl_group *rg, *crg;
	int cpu, ret = 0;

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
	if (r->infodir)
		resctrl_addinfofiles(r);

	if (r->type == RESCTRL_MONITOR) {
		if (r->mon_domain_dir) {
			list_for_each_entry(rg, &all_ctrl_groups, list) {
				resctrl_create_domain_files(rg->mondata, r, rg);
				list_for_each_entry(crg, &rg->child_list, list)
					resctrl_create_domain_files(crg->mondata, r, crg);
			}
		}
		if (r->mon_domain_file)
			arch_add_monitor(r->mon_event);
	}

	list_add(&r->list, &resctrl_all_resources);
out:
	mutex_unlock(&resctrl_mutex);
	cpus_read_unlock();

	return ret;
}
EXPORT_SYMBOL_GPL(resctrl_register_ctrl_resource);

void resctrl_unregister_ctrl_resource(struct resctrl_resource *r)
{
	struct resctrl_group *rg, *crg;
	int cpu;

	cpus_read_lock();
	mutex_lock(&resctrl_mutex);
	if (r->type == RESCTRL_MONITOR && r->mon_domain_file)
		arch_del_monitor(r->mon_event);

	if (r->mon_domain_dir) {
		list_for_each_entry(rg, &all_ctrl_groups, list) {
			resctrl_remove_domain_files(rg->mondata, r, rg);
			list_for_each_entry(crg, &rg->child_list, list)
				resctrl_remove_domain_files(crg->mondata, r, crg);
		}
	}
	if (r->infodir)
		resctrl_delinfofiles(r);
	if (r->domain_size)
		for_each_online_cpu(cpu)
			resctrl_domain_remove_cpu(cpu, r);
	list_del(&r->list);
	mutex_unlock(&resctrl_mutex);
	cpus_read_unlock();
}
EXPORT_SYMBOL_GPL(resctrl_unregister_ctrl_resource);
