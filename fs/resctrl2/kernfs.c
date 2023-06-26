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

struct kernfs_node *resctrl_add_file(struct kernfs_node *parent_kn, char *name, umode_t mode,
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
