// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2023 Intel Corporation. */

#include "internal.h"

static void create_one_file(struct resctrl_resource *r, struct resctrl_group *rg, int id)
{
	struct kernfs_node *parent_kn, *mon_data;
	struct resctrl_node_info *rni;
	struct mon_file_info *mfi;
	struct kernfs_node *kn;
	char name[20];

	/* Create "mon_data" directory if it isn't already there */
	rni = (struct resctrl_node_info *)rg - 1;
	parent_kn = rni->kn;
	mon_data = kernfs_find_and_get_ns(parent_kn, "mon_data", NULL);
	if (!mon_data)
		mon_data = resctrl_add_dir(parent_kn, "mon_data", NULL);
	if (!mon_data)
		return;
	rg->mondata = mon_data;

	sprintf(name, r->mon_domain_dir, id);
	kn = kernfs_find_and_get_ns(mon_data, name, NULL);
	if (!kn)
		kn = resctrl_add_dir(mon_data, name, NULL);

	rni = resctrl_add_file(kn, r->mon_domain_file, 0444, RESCTRL_MONFILE);
	if (!rni)
		return;

	mfi = (struct mon_file_info *)&rni->priv;
	mfi->domain_id = id;
	mfi->resctrl_ids = rg->resctrl_ids;
	mfi->show = r->mon_show;

	kernfs_activate(mon_data);
}

/*
 * New domain for this resource. Create files in each resource group.
 */
void resctrl_create_domain_files(struct resctrl_resource *r, struct resctrl_domain *d)
{
	struct resctrl_group *rg, *crg;

	list_for_each_entry(rg, &all_ctrl_groups, list) {
		create_one_file(r, rg, d->id);

		list_for_each_entry(crg, &rg->child_list, list)
			create_one_file(r, crg, d->id);
	}
}

/*
 * New resource group. Create files for each domain.
 */
void resctrl_create_all_domain_files(struct resctrl_resource *r, struct resctrl_group *rg)
{
	struct resctrl_domain *d;

	list_for_each_entry(d, &r->domains, list)
		create_one_file(r, rg, d->id);
}

void remove_one_file(struct resctrl_resource *r, struct resctrl_group *rg, int id,
		     struct list_head *h)
{
	struct kernfs_node *parent_kn = rg->mondata;
	struct resctrl_node_info *rni;
	struct kernfs_node *kn;
	char name[20];

	sprintf(name, r->mon_domain_dir, id);
	kn = kernfs_find_and_get_ns(parent_kn, name, NULL);
	if (!kn)
		return;
	kn = kernfs_find_and_get_ns(kn, r->mon_domain_file, NULL);
	if (!kn)
		return;
	rni = kn->priv;
	atomic_inc(&rni->waitcount);
	rni->flags |= RESCTRL_DELETED;
	kernfs_remove(kn);
	list_add(&rni->clean_list, h);
}

/*
 * A domain has gone away. Remove files for one resource.
 */
void resctrl_remove_domain_files(struct resctrl_resource *r, struct resctrl_domain *d,
				 struct list_head *h)
{
	struct resctrl_group *rg, *crg;

	list_for_each_entry(rg, &all_ctrl_groups, list) {
		remove_one_file(r, rg, d->id, h);

		list_for_each_entry(crg, &rg->child_list, list)
			remove_one_file(r, crg, d->id, h);
	}
}

/*
 * A resource group was removed. Remove files from each domain.
 */
void resctrl_remove_all_domain_files(struct resctrl_resource *r, struct resctrl_group *rg,
				     struct list_head *h)
{
	struct resctrl_domain *d;

	list_for_each_entry(d, &r->domains, list)
		remove_one_file(r, rg, d->id, h);
}
