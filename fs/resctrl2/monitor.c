// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2023 Intel Corporation. */

#include "internal.h"

void resctrl_create_domain_files(struct kernfs_node *parent_kn, struct resctrl_resource *r,
				 struct resctrl_group *rg)
{
	struct resctrl_node_info *rni;
	struct mon_file_info *mfi;
	struct resctrl_domain *d;
	struct kernfs_node *kn;
	char name[20];

	list_for_each_entry(d, &r->domains, list) {
		sprintf(name, r->mon_domain_dir, d->id);
		kn = kernfs_find_and_get_ns(parent_kn, name, NULL);
		if (!kn)
			kn = resctrl_add_dir(parent_kn, name, NULL);

		rni = resctrl_add_file(kn, r->mon_domain_file, 0444, RESCTRL_MONFILE);
		if (!rni)
			break;

		mfi = (struct mon_file_info *)&rni->priv;
		mfi->domain_id = d->id;
		mfi->resctrl_ids = rg->resctrl_ids;
		mfi->show = r->mon_show;
	}
	kernfs_activate(parent_kn);
}

void resctrl_remove_domain_files(struct kernfs_node *parent_kn, struct resctrl_resource *r,
				 struct list_head *h)
{
	struct resctrl_node_info *rni;
	struct resctrl_domain *d;
	struct kernfs_node *kn;
	char name[20];

	list_for_each_entry(d, &r->domains, list) {
		sprintf(name, r->mon_domain_dir, d->id);
		kn = kernfs_find_and_get_ns(parent_kn, name, NULL);
		if (!kn)
			continue;
		kn = kernfs_find_and_get_ns(kn, r->mon_domain_file, NULL);
		if (!kn)
			continue;
		rni = kn->priv;
		atomic_inc(&rni->waitcount);
		rni->flags |= RESCTRL_DELETED;
		kernfs_remove(kn);
		list_add(&rni->clean_list, h);
	}
}
