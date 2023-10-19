/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright(c) 2023 Intel Corporation. */

#include <linux/cpu.h>
#include <linux/kernfs.h>
#include <linux/resctrl.h>
#include <linux/seq_buf.h>
#include <linux/seq_file.h>

enum directory_type {
	DIR_INFO,
	DIR_ROOT,
	DIR_CTRL_MON,
};

struct resctrl_group {
	enum directory_type	type;
	resctrl_ids_t		resctrl_ids;
	struct list_head	list;
	struct resctrl_group	*parent;
	struct list_head	child_list;
};

#include <asm/resctrl.h>

#define for_each_resource(r)					\
	list_for_each_entry(r, &resctrl_all_resources, list)

#define for_each_resource_by_cap(r, capability)			\
	for_each_resource(r)					\
		if (r->capability)

struct resctrl_node_info {
	int			type;
	int			flags;
	struct list_head	clean_list;
	struct kernfs_node	*kn;
	atomic_t		waitcount;
	u64			priv[];
};

/* resctrl_node_info.flags */
#define RESCTRL_DELETED		BIT(0)
#define RESCTRL_LOCK_CPUS	BIT(1)

#define RESCTRL_INFOFILE	2
struct info_file_info {
	struct resctrl_resource *r;
	int			(*show)(struct seq_file *sf);
};

#define RESCTRL_COREFILE	3
struct core_file_info {
	struct resctrl_group	*rg;
	int			(*show)(struct seq_file *sf, struct resctrl_group *rg);
	ssize_t			(*write)(char *buf, size_t nbytes, struct resctrl_group *rg,
					 struct kernfs_open_file *of);
};

#define RESCTRL_GROUP		5
// Used for control and monitor directories. priv[] is struct resctrl_group

// Macros to check if struct kernfs_node->priv is being used as a reference
// counter instead of pointer to custom data
#define RESCTRL_MAX_REF_COUNT	10000
#define IS_RESCTRL_REFCOUNT(priv) ((unsigned long)(priv) <= RESCTRL_MAX_REF_COUNT)

// cpu.c
int resctrl_cpu_init(void);
void resctrl_cpu_exit(void);

// directory.c
int resctrl_mkdir(struct kernfs_node *parent_kn, const char *name, umode_t mode);
int resctrl_rmdir(struct kernfs_node *kn);
void resctrl_rmdir_all_sub(bool is_umount, struct list_head *h);
bool resctrl_populate_dir(struct kernfs_node *parent_kn, struct resctrl_group *rg);

// domain.c
void resctrl_domain_add_cpu(unsigned int cpu, struct resctrl_resource *r);
void resctrl_domain_remove_cpu(unsigned int cpu, struct resctrl_resource *r);

// info.c
bool resctrl_add_info_dir(struct kernfs_node *parent_kn);
void resctrl_addinfofiles(struct resctrl_resource *r);
void resctrl_delinfofiles(struct resctrl_resource *r, struct list_head *h);
void resctrl_last_cmd_clear(void);
void resctrl_last_cmd_puts(const char *s);
void resctrl_last_cmd_printf(const char *fmt, ...);

// kernfs.c
struct kernfs_node *__resctrl_add_file(struct kernfs_node *parent_kn, char *name, umode_t mode,
				       const struct kernfs_ops *ops, void *priv);
struct kernfs_node *resctrl_add_dir(struct kernfs_node *parent_kn, const char *name,
				    void *priv);
struct resctrl_node_info *resctrl_add_file(struct kernfs_node *parent_kn, char *name,
					   umode_t mode, int type);
void resctrl_remove_file(char *name, struct kernfs_node *parent_kn, struct list_head *h);
void resctrl_node_remove(struct resctrl_node_info *rni);
void resctrl_node_file_cleanup(struct list_head *h);

// locking.c
extern struct mutex resctrl_mutex;
void resctrl_kn_get(struct resctrl_node_info *rni, struct kernfs_node *kn);
void resctrl_kn_put(struct resctrl_node_info *rni, struct kernfs_node *kn);
struct resctrl_node_info *resctrl_kn_lock_live(struct kernfs_node *kn);
void resctrl_kn_unlock(struct kernfs_node *kn);

// resources.c
extern struct list_head resctrl_all_resources;
void resctrl_activate(struct resctrl_resource *r);
void resctrl_deactivate(struct resctrl_resource *r, bool is_umount, struct list_head *h);

// root.c
extern struct resctrl_group *resctrl_default;
extern bool resctrl_is_mounted;
extern struct list_head all_ctrl_groups;

// schemata.c
bool resctrl_add_schemata_file(struct kernfs_node *parent_kn);
void resctrl_remove_schemata_file(struct kernfs_node *parent_kn, struct list_head *h);

// tasks.c
bool resctrl_add_task_file(struct kernfs_node *parent_kn);
void resctrl_remove_task_file(struct kernfs_node *parent_kn, struct list_head *h);
void resctrl_move_group_tasks(struct resctrl_group *from, struct resctrl_group *to);
