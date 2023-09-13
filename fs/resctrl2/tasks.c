// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2023 Intel Corporation. */

#include "internal.h"

static void show_resctrl_tasks(struct resctrl_group *rg, struct seq_file *s)
{
	struct task_struct *p, *t;
	pid_t pid;

	rcu_read_lock();
	for_each_process_thread(p, t) {
		if (arch_is_resctrl_id_match(t, rg)) {
			pid = task_pid_vnr(t);
			if (pid)
				seq_printf(s, "%d\n", t->pid);
		}
	}
	rcu_read_unlock();
}

static int tasks_seq_show(struct seq_file *m, struct resctrl_group *rg)
{
	show_resctrl_tasks(rg, m);

	return 0;
}

bool resctrl_add_task_file(struct kernfs_node *parent_kn)
{
	struct resctrl_node_info *rni, *prni;
	struct core_file_info *cfi;

	rni = resctrl_add_file(parent_kn, "tasks", 0644, RESCTRL_COREFILE);
	if (!rni)
		return false;
	prni = parent_kn->priv;
	cfi = (struct core_file_info *)&rni->priv;
	cfi->rg = (struct resctrl_group *)&prni->priv;
	cfi->show = tasks_seq_show;

	return true;
}

void resctrl_remove_task_file(struct kernfs_node *parent_kn, struct list_head *h)
{
	resctrl_remove_file("tasks", parent_kn, h);
}
