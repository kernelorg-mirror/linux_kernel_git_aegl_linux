// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2023 Intel Corporation. */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs_context.h>
#include <linux/user_namespace.h>
#include <linux/fs_parser.h>
#include <linux/resctrl.h>

#include "internal.h"

#undef pr_fmt
#define pr_fmt(fmt)       KBUILD_MODNAME ": " fmt

#define RESCTRL_SUPER_MAGIC 0x4145474C

static struct kernfs_root *resctrl_root;

struct resctrl_fs_context {
	struct kernfs_fs_context kfc;
};

LIST_HEAD(all_ctrl_groups);
bool resctrl_is_mounted;

static struct resctrl_node_info *resctrl_default_rni;
struct resctrl_group *resctrl_default;

static void resctrl_fs_context_free(struct fs_context *fc)
{
	struct kernfs_fs_context *kfc = fc->fs_private;
	struct resctrl_fs_context *ctx = container_of(kfc, struct resctrl_fs_context, kfc);

	kernfs_free_fs_context(fc);
	kfree(ctx);
}

static const struct fs_parameter_spec resctrl_fs_parameters[] = {
	{}
};

static int resctrl_parse_param(struct fs_context *fc, struct fs_parameter *param)
{
	return 0;
}

static int resctrl_get_tree(struct fs_context *fc)
{
	struct resctrl_resource *r;
	int ret = 0;

	cpus_read_lock();
	mutex_lock(&resctrl_mutex);

	/* resctrl file system can only be mounted once. */
	if (resctrl_is_mounted) {
		ret = -EBUSY;
		goto unlock;
	}

	ret = kernfs_get_tree(fc);
	if (ret || !resctrl_populate_dir(resctrl_default_rni->kn, resctrl_default)) {
		ret = -ENOSPC;
		goto unlock;
	}
	kernfs_activate(resctrl_default_rni->kn);

	for_each_resource(r)
		resctrl_activate(r);

	static_branch_enable_cpuslocked(&resctrl_enable_key);
	resctrl_is_mounted = true;
unlock:
	mutex_unlock(&resctrl_mutex);

	cpus_read_unlock();
	return ret;
}

static const struct fs_context_operations resctrl_fs_context_ops = {
	.free		= resctrl_fs_context_free,
	.parse_param	= resctrl_parse_param,
	.get_tree	= resctrl_get_tree,
};

static struct kernfs_syscall_ops resctrl_kf_syscall_ops = {
	.mkdir	= resctrl_mkdir,
	.rmdir	= resctrl_rmdir,
	.rename	= resctrl_rename,
};

static int resctrl_init_fs_context(struct fs_context *fc)
{
	struct resctrl_fs_context *ctx;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->kfc.root = resctrl_root;
	ctx->kfc.magic = RESCTRL_SUPER_MAGIC;
	fc->fs_private = &ctx->kfc;
	fc->ops = &resctrl_fs_context_ops;
	put_user_ns(fc->user_ns);
	fc->user_ns = get_user_ns(&init_user_ns);
	fc->global = true;

	return 0;
}

static void resctrl_kill_sb(struct super_block *sb)
{
	LIST_HEAD(mon_file_clean_list);
	struct resctrl_resource *r;

	cpus_read_lock();
	mutex_lock(&resctrl_mutex);

	for_each_resource(r)
		resctrl_deactivate(r, &mon_file_clean_list);

	resctrl_move_group_tasks(NULL, resctrl_default, NULL);
	resctrl_rmdir_all_sub(&mon_file_clean_list);
	static_branch_disable_cpuslocked(&resctrl_enable_key);
	kernfs_kill_sb(sb);

	resctrl_is_mounted = false;
	mutex_unlock(&resctrl_mutex);
	cpus_read_unlock();
	resctrl_node_file_cleanup(&mon_file_clean_list);
}

static struct file_system_type resctrl_fs_type = {
	.name			= "resctrl",
	.init_fs_context	= resctrl_init_fs_context,
	.parameters		= resctrl_fs_parameters,
	.kill_sb		= resctrl_kill_sb,
};

static int __init resctrl_setup_root(void)
{
	resctrl_root = kernfs_create_root(&resctrl_kf_syscall_ops,
					  KERNFS_ROOT_CREATE_DEACTIVATED |
					  KERNFS_ROOT_EXTRA_OPEN_PERM_CHECK,
					  resctrl_default_rni);
	if (IS_ERR(resctrl_root))
		return PTR_ERR(resctrl_root);

	resctrl_default->resctrl_ids = arch_resctrl_default_ids;
	resctrl_default_rni->kn = kernfs_root_to_node(resctrl_root);
	resctrl_default_rni->type = RESCTRL_GROUP;
	resctrl_default->type = DIR_ROOT;
	resctrl_default->mode = RESCTRL_SHARED;
	INIT_LIST_HEAD(&resctrl_default->child_list);

	list_add(&resctrl_default->list, &all_ctrl_groups);

	if (!resctrl_add_info_dir(resctrl_default_rni->kn))
		return -EINVAL;

	kernfs_activate(resctrl_default_rni->kn);

	return 0;
}

static int resctrl_init(void)
{
	int ret;

	if (!arch_check_resctrl_support())
		return -EINVAL;

	resctrl_default_rni = kzalloc(sizeof(*resctrl_default_rni) + sizeof(*resctrl_default),
				      GFP_KERNEL);
	if (!resctrl_default_rni)
		return -ENOMEM;
	resctrl_default = (struct resctrl_group *)resctrl_default_rni->priv;

	ret = resctrl_cpu_init();
	if (ret < 0)
		goto free;

	ret = resctrl_setup_root();
	if (ret)
		goto cpu_exit;

	ret = sysfs_create_mount_point(fs_kobj, "resctrl");
	if (ret)
		goto cleanup_root;

	ret = register_filesystem(&resctrl_fs_type);
	if (ret)
		goto cleanup_mountpoint;

	return 0;

cleanup_mountpoint:
	sysfs_remove_mount_point(fs_kobj, "resctrl");
cleanup_root:
	kernfs_destroy_root(resctrl_root);
cpu_exit:
	resctrl_cpu_exit();
free:
	kfree(resctrl_default_rni);

	return ret;
}

fs_initcall(resctrl_init);

MODULE_LICENSE("GPL");
