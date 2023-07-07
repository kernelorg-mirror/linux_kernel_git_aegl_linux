// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2023 Intel Corporation. */

#include "internal.h"

/* Set uid and gid of dirs and files to that of the creator */
static int kn_set_ugid(struct kernfs_node *kn)
{
	struct iattr iattr = { .ia_valid = ATTR_UID | ATTR_GID,
				.ia_uid = current_fsuid(),
				.ia_gid = current_fsgid(), };

	if (uid_eq(iattr.ia_uid, GLOBAL_ROOT_UID) &&
	    gid_eq(iattr.ia_gid, GLOBAL_ROOT_GID))
		return 0;

	return kernfs_setattr(kn, &iattr);
}

struct kernfs_node *__resctrl_add_file(struct kernfs_node *parent_kn, char *name, umode_t mode,
				     const struct kernfs_ops *ops, void *priv)
{
	struct kernfs_node *kn;
	int ret;

	kn = __kernfs_create_file(parent_kn, name, mode,
				  GLOBAL_ROOT_UID, GLOBAL_ROOT_GID,
				  0, ops, priv, NULL, NULL);
	if (IS_ERR(kn))
		return NULL;

	ret = kn_set_ugid(kn);
	if (ret) {
		kernfs_remove(kn);
		return NULL;
	}

	return kn;
}

struct resctrl_node_info *resctrl_add_file(struct kernfs_node *parent_kn, char *name,
					   umode_t mode, int type)
{
	struct resctrl_node_info *rni;
	int size = sizeof(*rni);
	struct kernfs_node *kn;

	switch (type) {
	case RESCTRL_MONFILE: size += sizeof(struct mon_file_info); break;
	case RESCTRL_INFOFILE: size += sizeof(struct info_file_info); break;
	case RESCTRL_COREFILE: size += sizeof(struct core_file_info); break;
	default:
		return NULL;
	}
	rni = kzalloc(size, GFP_KERNEL);
	if (!rni)
		return NULL;
	rni->type = type;
	kn = __resctrl_add_file(parent_kn, name, mode, &resctrl_file_ops, rni);
	if (!kn)
		return NULL;
	rni->kn = kn;
	kernfs_get(kn);

	return rni;
}

void resctrl_remove_file(char *name, struct kernfs_node *parent_kn, struct list_head *h)
{
	struct resctrl_node_info *rni;
	struct kernfs_node *kn;

	kn = kernfs_find_and_get_ns(parent_kn, name, NULL);
	if (kn) {
		rni = kn->priv;
		atomic_inc(&rni->waitcount);
		rni->flags |= RESCTRL_DELETED;
		kernfs_remove(kn);
		list_add(&rni->clean_list, h);
	}
}

struct kernfs_node *resctrl_add_dir(struct kernfs_node *parent_kn, const char *name,
				    void *priv)
{
	struct kernfs_node *kn;
	int ret;

	kn = kernfs_create_dir(parent_kn, name, parent_kn->mode, priv);
	if (IS_ERR(kn))
		return NULL;

	ret = kn_set_ugid(kn);
	if (ret) {
		kernfs_remove(kn);
		return NULL;
	}

	return kn;
}

void resctrl_node_remove(struct resctrl_node_info *rni)
{
	kernfs_put(rni->kn);
	kfree(rni);
}

void resctrl_node_file_cleanup(struct list_head *h)
{
	struct resctrl_node_info *rni, *tmp;

	list_for_each_entry_safe(rni, tmp, h, clean_list) {
		if (atomic_dec_and_test(&rni->waitcount) &&
		    (rni->flags & RESCTRL_DELETED)) {
			kernfs_unbreak_active_protection(rni->kn);
			resctrl_node_remove(rni);
		}
		list_del(&rni->clean_list);
	}
}

static int resctrl_file_show(struct seq_file *sf, void *v)
{
	struct kernfs_open_file *of = sf->private;
	struct resctrl_node_info *rni;
	struct info_file_info *ifi;
	struct core_file_info *cfi;
	struct mon_file_info *mfi;
	int ret;

	rni = resctrl_kn_lock_live(of->kn);

	if (!rni) {
		ret = -ENOENT;
		goto out;
	}

	switch (rni->type) {
	case RESCTRL_MONFILE:
		mfi = (struct mon_file_info *)&rni->priv;
		ret = mfi->show(sf, mfi->domain_id, mfi->resctrl_ids);
		break;
	case RESCTRL_INFOFILE:
		ifi = (struct info_file_info *)&rni->priv;
		ret = ifi->show(sf, ifi->r);
		break;
	case RESCTRL_COREFILE:
		cfi = (struct core_file_info *)&rni->priv;
		ret = cfi->show(sf, cfi->rg);
		break;
	default:
		ret = -EOPNOTSUPP;
		break;
	}
out:
	resctrl_kn_unlock(of->kn);

	return ret;
}

static ssize_t resctrl_file_write(struct kernfs_open_file *of, char *buf,
				  size_t nbytes, loff_t off)
{
	struct resctrl_node_info *rni;
	struct info_file_info *ifi;
	struct core_file_info *cfi;
	int ret;

	rni = resctrl_kn_lock_live(of->kn);

	if (!rni) {
		ret = -ENOENT;
		goto out;
	}

	switch (rni->type) {
	case RESCTRL_INFOFILE:
		ifi = (struct info_file_info *)&rni->priv;
		ret = ifi->write(buf, nbytes);
		break;
	case RESCTRL_COREFILE:
		cfi = (struct core_file_info *)&rni->priv;
		ret = cfi->write(buf, nbytes, cfi->rg, of);
		break;
	default:
		ret = -EOPNOTSUPP;
		break;
	}
out:
	resctrl_kn_unlock(of->kn);

	return ret ?: nbytes;
}

struct kernfs_ops resctrl_file_ops = {
	.atomic_write_len	= PAGE_SIZE,
	.write			= resctrl_file_write,
	.seq_show		= resctrl_file_show,
	.poll			= kernfs_generic_poll,
};
