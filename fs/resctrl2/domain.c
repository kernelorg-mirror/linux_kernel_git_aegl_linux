// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2023 Intel Corporation. */

#include <linux/cacheinfo.h>
#include "internal.h"

/*
 * find_domain - Find a domain in a resource that matches input resource id
 *
 * Search resource r's domain list to find the resource id. If the resource
 * id is found in a domain, return the domain. Otherwise, if requested by
 * caller, return the first domain whose id is bigger than the input id.
 * The domain list is sorted by id in ascending order.
 */
static struct resctrl_domain *find_domain(struct resctrl_resource *r, int id,
					  struct list_head **pos)
{
	struct resctrl_domain *d;
	struct list_head *l;

	if (id < 0)
		return ERR_PTR(-ENODEV);

	list_for_each(l, &r->domains) {
		d = list_entry(l, struct resctrl_domain, list);
		/* When id is found, return its domain. */
		if (id == d->id)
			return d;
		/* Stop searching when finding id's position in sorted list. */
		if (id < d->id)
			break;
	}

	if (pos)
		*pos = l;

	return NULL;
}

static int get_domain_id(unsigned int cpu, enum resctrl_scope scope)
{
	switch (scope) {
	case RESCTRL_CORE: return topology_core_id(cpu);
	case RESCTRL_L2CACHE: return get_cpu_cacheinfo_id(cpu, 2);
	case RESCTRL_L3CACHE: return get_cpu_cacheinfo_id(cpu, 3);
	case RESCTRL_SOCKET: return topology_physical_package_id(cpu);
	}
	return -1;
}

static int get_cpu_cache_size(int cpu, int cache_level)
{
	struct cpu_cacheinfo *ci;

	ci = get_cpu_cacheinfo(cpu);
	for (int i = 0; i < ci->num_leaves; i++) {
		if (ci->info_list[i].level == cache_level) {
			return ci->info_list[i].size;
		}
	}

	return 0;
}

void resctrl_domain_add_cpu(unsigned int cpu, struct resctrl_resource *r)
{
	int id = get_domain_id(cpu, r->scope);
	struct list_head *add_pos = NULL;
	struct resctrl_domain *d;

	d = find_domain(r, id, &add_pos);
	if (IS_ERR(d)) {
		pr_warn("Couldn't find domain id for CPU %d\n", cpu);
		return;
	}

	if (d) {
		cpumask_set_cpu(cpu, &d->cpu_mask);
		r->domain_update(r, RESCTRL_DOMAIN_ADD_CPU, cpu, d);
		return;
	}

	d = kzalloc_node(r->domain_size, GFP_KERNEL, cpu_to_node(cpu));
	if (!d)
		return;

	d->id = id;
	if (r->scope == RESCTRL_L2CACHE)
		d->cache_size = get_cpu_cache_size(cpu, 2);
	else if (r->scope == RESCTRL_L3CACHE)
		d->cache_size = get_cpu_cache_size(cpu, 3);

	cpumask_set_cpu(cpu, &d->cpu_mask);
	if (r->mon_domain_dir)
		resctrl_create_domain_files(r, d);
	r->domain_update(r, RESCTRL_DOMAIN_ADD, cpu, d);

	list_add_tail(&d->list, add_pos);
}

void resctrl_domain_remove_cpu(unsigned int cpu, struct resctrl_resource *r,
			       struct list_head *h)
{
	int  id;
	struct resctrl_domain *d;

	id = get_domain_id(cpu, r->scope);
	d = find_domain(r, id, NULL);
	if (IS_ERR_OR_NULL(d)) {
		pr_warn("Couldn't find domain id for CPU %d\n", cpu);
		return;
	}

	cpumask_clear_cpu(cpu, &d->cpu_mask);
	if (cpumask_empty(&d->cpu_mask)) {
		if (r->mon_domain_dir)
			resctrl_remove_domain_files(r, d, h);
		r->domain_update(r, RESCTRL_DOMAIN_DELETE, cpu, d);
		list_del(&d->list);
		kfree(d);
	} else {
		r->domain_update(r, RESCTRL_DOMAIN_DELETE_CPU, cpu, d);
	}
}
