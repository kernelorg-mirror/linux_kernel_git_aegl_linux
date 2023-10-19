/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright(c) 2023 Intel Corporation. */

#include <linux/cpu.h>
#include <linux/kernfs.h>
#include <linux/resctrl.h>
#include <linux/seq_buf.h>
#include <linux/seq_file.h>

enum directory_type {
	DIR_INFO,
};

struct resctrl_group {
	enum directory_type	type;
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

#define RESCTRL_INFOFILE	2
struct info_file_info {
	struct resctrl_resource *r;
	int			(*show)(struct seq_file *sf);
};

// cpu.c
int resctrl_cpu_init(void);
void resctrl_cpu_exit(void);

// domain.c
void resctrl_domain_add_cpu(unsigned int cpu, struct resctrl_resource *r);
void resctrl_domain_remove_cpu(unsigned int cpu, struct resctrl_resource *r);

// info.c
bool resctrl_add_info_dir(struct kernfs_node *parent_kn);
void resctrl_addinfofiles(struct resctrl_resource *r);
void resctrl_delinfofiles(struct resctrl_resource *r, struct list_head *h);

// kernfs.c
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
void resctrl_deactivate(struct resctrl_resource *r, struct list_head *h);

// root.c
extern bool resctrl_is_mounted;
