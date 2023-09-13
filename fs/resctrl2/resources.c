// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2023 Intel Corporation. */

#include "internal.h"

LIST_HEAD(resctrl_all_resources);

void resctrl_activate(struct resctrl_resource *r)
{
	if (r->infodir)
		resctrl_addinfofiles(r);
}

int resctrl_register_resource(struct resctrl_resource *r)
{
	struct resctrl_resource *rr;
	int ret;
	int cpu;

	cpus_read_lock();
	mutex_lock(&resctrl_mutex);

	if (r->num_alloc_ids) {
		ret = arch_init_alloc_ids(r);
		if (ret < 0)
			goto out;
		if (ret)
			for_each_resource_by_cap(rr, num_alloc_ids)
				rr->num_alloc_ids = r->num_alloc_ids;
	}

	ret = 0;

	if (r->domain_size) {
		if (r->domain_size < sizeof(struct resctrl_domain)) {
			pr_warn("Resource %s: domain_size too small\n", r->name);
			ret = -EINVAL;
			goto out;
		}
		for_each_online_cpu(cpu)
			resctrl_domain_add_cpu(cpu, r);
	}

	if (resctrl_is_mounted)
		resctrl_activate(r);

	list_add(&r->list, &resctrl_all_resources);
out:
	mutex_unlock(&resctrl_mutex);
	cpus_read_unlock();

	return ret;
}
EXPORT_SYMBOL_GPL(resctrl_register_resource);

void resctrl_deactivate(struct resctrl_resource *r, bool is_umount, struct list_head *h)
{
	if (r->infodir)
		resctrl_delinfofiles(r, h);

	if (r->num_alloc_ids) {
		struct resctrl_resource *rr;
		bool do_reset = true;

		for_each_resource_by_cap(rr, num_alloc_ids) {
			if (rr == r)
				continue;
			do_reset = false;
			break;
		}
		if (do_reset) {
			resctrl_rmdir_all_sub(is_umount, h);
			arch_reset_alloc_ids();
		}
	}
}

void resctrl_unregister_resource(struct resctrl_resource *r)
{
	LIST_HEAD(clean_list);
	int cpu;

	cpus_read_lock();
	mutex_lock(&resctrl_mutex);

	if (resctrl_is_mounted)
		resctrl_deactivate(r, false, &clean_list);

	if (r->domain_size)
		for_each_online_cpu(cpu)
			resctrl_domain_remove_cpu(cpu, r);

	list_del(&r->list);

	mutex_unlock(&resctrl_mutex);
	cpus_read_unlock();

	resctrl_node_file_cleanup(&clean_list);
}
EXPORT_SYMBOL_GPL(resctrl_unregister_resource);
