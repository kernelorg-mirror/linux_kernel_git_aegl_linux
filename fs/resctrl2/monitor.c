// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2023 Intel Corporation. */

#include "internal.h"

struct mon_file_info {
	int			domain_id;
	u64			resctrl_ids;
	int			flags;
	atomic_t		waitcount;
	struct kernfs_node	*kn;
	int			(*show)(struct seq_file *sf, int domain_id, u64 resctrl_ids);
	struct list_head	list;
};

static void monfile_remove(struct mon_file_info *mfi)
{
	kernfs_put(mfi->kn);
	kfree(mfi);
}

static int mon_file_show(struct seq_file *sf, void *v)
{
	struct kernfs_open_file *of = sf->private;
	struct mon_file_info *mfi = of->kn->priv;
	int ret;

	atomic_inc(&mfi->waitcount);
	kernfs_break_active_protection(of->kn);

	mutex_lock(&resctrl_mutex);

	if (mfi->flags & RESCTRL_DELETED) {
		ret = -ENOENT;
		goto out;
	}

	ret = mfi->show(sf, mfi->domain_id, mfi->resctrl_ids);
out:
	mutex_unlock(&resctrl_mutex);

	if (atomic_dec_and_test(&mfi->waitcount) &&
	    (mfi->flags & RESCTRL_DELETED)) {
		kernfs_unbreak_active_protection(of->kn);
		monfile_remove(mfi);
	} else {
		kernfs_unbreak_active_protection(of->kn);
	}

	return ret;
}

static struct kernfs_ops mon_file_ops = {
	.seq_show	= mon_file_show,
};

void resctrl_create_domain_files(struct kernfs_node *parent_kn, struct resctrl_resource *r,
				 struct resctrl_group *rg)
{
	struct mon_file_info *mfi;
	struct resctrl_domain *d;
	struct kernfs_node *kn;
	char name[20];

	list_for_each_entry(d, &r->domains, list) {
		mfi = kzalloc(sizeof(*mfi), GFP_KERNEL);
		if (!mfi)
			break;

		sprintf(name, r->mon_domain_dir, d->id);
		kn = kernfs_find_and_get_ns(parent_kn, name, NULL);
		if (!kn)
			kn = resctrl_add_dir(parent_kn, name, NULL);

		mfi->domain_id = d->id;
		mfi->resctrl_ids = rg->resctrl_ids;
		mfi->show = r->mon_show;
		mfi->kn = resctrl_add_file(kn, r->mon_domain_file, 0444, &mon_file_ops, mfi);
		kernfs_get(mfi->kn);
	}
	kernfs_activate(parent_kn);
}

void resctrl_remove_domain_files(struct kernfs_node *parent_kn, struct resctrl_resource *r,
				 struct list_head *h)
{
	struct mon_file_info *mfi;
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
		mfi = kn->priv;
		atomic_inc(&mfi->waitcount);
		mfi->flags = RESCTRL_DELETED;
		kernfs_remove(kn);
		list_add(&mfi->list, h);
	}
}

void resctrl_mon_file_cleanup(struct list_head *h)
{
	struct mon_file_info *mfi, *tmp;

	list_for_each_entry_safe(mfi, tmp, h, list) {
		if (atomic_dec_and_test(&mfi->waitcount) &&
		    (mfi->flags & RESCTRL_DELETED)) {
			kernfs_unbreak_active_protection(mfi->kn);
			monfile_remove(mfi);
		}
		list_del(&mfi->list);
	}
}
