/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright(c) 2023 Intel Corporation. */

#include <linux/cpu.h>
#include <linux/kernfs.h>
#include <linux/resctrl.h>
#include <linux/seq_buf.h>
#include <linux/seq_file.h>

#undef pr_fmt
#define pr_fmt(fmt) "resctrl2: " fmt

enum directory_type {
	DIR_ROOT,
	DIR_CTRL_MON,
	DIR_MONGROUP,
	DIR_MONDATA,
	DIR_MON,
	DIR_INFO,
};

struct resctrl_group {
	enum directory_type	type;
	atomic_t		waitcount;

	struct kernfs_node	*kn;
	u32			flags;
	u64			resctrl_ids;
	struct list_head	list;

	struct resctrl_group	*parent;
	struct list_head	child_list;
	struct kernfs_node	*mondata;

	struct cpumask		cpu_mask;
	enum resctrl_mode	mode;
};

#include <asm/resctrl.h>

extern struct resctrl_group resctrl_default;

/* resctrl_group.flags */
#define RESCTRL_DELETED	1

#define for_each_resource(r)						\
	list_for_each_entry(r, &resctrl_all_resources, list)

#define for_each_control_resource(r)					\
	list_for_each_entry(r, &resctrl_all_resources, list)		\
		if (r->type == RESCTRL_CONTROL)

#define for_each_monitor_resource(r)					\
	list_for_each_entry(r, &resctrl_all_resources, list)		\
		if (r->type == RESCTRL_MONITOR)

// cpu.c
int resctrl_cpu_init(void);
void resctrl_cpu_exit(void);
bool resctrl_add_cpus_file(struct kernfs_node *parent_kn);
void update_resctrl_ids(const struct cpumask *cpu_mask, struct resctrl_group *r);

// directory.c
int resctrl_mkdir(struct kernfs_node *parent_kn, const char *name, umode_t mode);
int resctrl_rmdir(struct kernfs_node *kn);
void resctrl_rmdir_all_sub(void);
bool resctrl_populate_dir(struct kernfs_node *parent_kn, struct resctrl_group *rg);
void resctrl_create_domain_files(struct kernfs_node *parent_kn, struct resctrl_resource *r,
				 struct resctrl_group *rg);
void resctrl_remove_domain_files(struct kernfs_node *parent_kn, struct resctrl_resource *r,
				 struct resctrl_group *rg);
void resctrl_group_remove(struct resctrl_group *rg);

// domain.c
void resctrl_domain_add_cpu(unsigned int cpu, struct resctrl_resource *r);
void resctrl_domain_remove_cpu(unsigned int cpu, struct resctrl_resource *r);

// info.c
bool resctrl_add_info_dir(struct kernfs_node *parent_kn);
void resctrl_addinfofiles(struct resctrl_resource *r);
void resctrl_delinfofiles(struct resctrl_resource *r);
void resctrl_last_cmd_clear(void);
void resctrl_last_cmd_puts(const char *s);
void resctrl_last_cmd_printf(const char *fmt, ...);

// kernfs.c
struct kernfs_node *resctrl_add_file(struct kernfs_node *parent_kn, char *name, umode_t mode,
				     const struct kernfs_ops *ops, void *priv);
struct kernfs_node *resctrl_add_dir(struct kernfs_node *parent_kn, const char *name,
				    void *priv);

// locking.c
struct resctrl_group *resctrl_group_kn_lock_live(struct kernfs_node *kn);
void resctrl_group_kn_unlock(struct kernfs_node *kn);
struct resctrl_group *kernfs_to_resctrl_group(struct kernfs_node *kn);

extern struct mutex resctrl_mutex;

// mode.c
bool resctrl_add_mode_file(struct kernfs_node *parent_kn);

// resources.c
extern struct list_head resctrl_all_resources;

// root.c
extern struct list_head all_ctrl_groups;

// schemata.c
bool resctrl_add_schemata_file(struct kernfs_node *parent_kn);

// tasks.c
bool resctrl_add_task_file(struct kernfs_node *parent_kn);
void resctrl_move_group_tasks(struct resctrl_group *from, struct resctrl_group *to,
			      struct cpumask *mask);
