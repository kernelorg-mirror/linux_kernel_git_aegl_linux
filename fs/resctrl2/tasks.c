// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2023 Intel Corporation. */

#include "internal.h"

/*
 * Interrupt running tasks to make sure that update to
 * new alloc/monitor ids.
 */
static void _update_task_resctrl_ids(void *task)
{
	/*
	 * If the task is still current on this CPU, update.
	 * Otherwise, the update will happen next time the
	 * task is scheduled in.
	 */
	if (task == current)
		resctrl_sched_in(task);
}

/*
 * Move tasks from one to the other group. If @from is NULL, then all tasks
 * in the systems are moved unconditionally (used for teardown).
 *
 * If @mask is not NULL the cpus on which moved tasks are running are set
 * in that mask so the update smp function call is restricted to affected
 * cpus.
 */
void resctrl_move_group_tasks(struct resctrl_group *from, struct resctrl_group *to,
			      struct cpumask *mask)
{
	struct task_struct *p, *t;

	read_lock(&tasklist_lock);
	for_each_process_thread(p, t) {
		if (!from || arch_is_resctrl_id_match(t, from)) {
			/* Change ID in task structure first */
			arch_set_task_ids(t, to);

			/* Ensure above update is visible */
			smp_mb();

			/*
			 * If the task is on a CPU, set the CPU in the mask.
			 * The detection is inaccurate as tasks might move or
			 * schedule before the smp function call takes place.
			 * In such a case the function call is pointless, but
			 * there is no other side effect.
			 */
			if (IS_ENABLED(CONFIG_SMP) && mask && task_curr(t))
				cpumask_set_cpu(task_cpu(t), mask);
		}
	}
	read_unlock(&tasklist_lock);
}

static int __resctrl_move_task(struct task_struct *tsk,
			       struct resctrl_group *rg)
{
	/* If the task is already in group, no need to move the task. */
	if (tsk->resctrl_ids == rg->resctrl_ids)
		return 0;

	/* Change ID in task structure first */
	if (!arch_set_task_ids(tsk, rg))
		return -EINVAL;

	/* Ensure above update is visible before kicking task */
	smp_mb();

	/*
	 * By now, the task's resctrl ids are set. If the task is current
	 * on a CPU, need to kick the task to make the ids take effect.
	 * If the task is not current, the update will happen when the
	 * task is scheduled in.
	 */
	if (IS_ENABLED(CONFIG_SMP) && task_curr(tsk))
		smp_call_function_single(task_cpu(tsk), _update_task_resctrl_ids, tsk, 1);

	return 0;
}

static int resctrl_task_write_permission(struct task_struct *task,
					 struct kernfs_open_file *of)
{
	const struct cred *tcred = get_task_cred(task);
	const struct cred *cred = current_cred();
	int ret = 0;

	/*
	 * Even if we're attaching all tasks in the thread group, we only
	 * need to check permissions on one of them.
	 */
	if (!uid_eq(cred->euid, GLOBAL_ROOT_UID) &&
	    !uid_eq(cred->euid, tcred->uid) &&
	    !uid_eq(cred->euid, tcred->suid)) {
		resctrl_last_cmd_printf("No permission to move task %d\n", task->pid);
		ret = -EPERM;
	}

	put_cred(tcred);
	return ret;
}

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

static int resctrl_move_task(pid_t pid, struct resctrl_group *rg, struct kernfs_open_file *of)
{
	struct task_struct *tsk;
	int ret;

	rcu_read_lock();
	if (pid) {
		tsk = find_task_by_vpid(pid);
		if (!tsk) {
			rcu_read_unlock();
			resctrl_last_cmd_printf("No task %d\n", pid);
			return -ESRCH;
		}
	} else {
		tsk = current;
	}

	get_task_struct(tsk);
	rcu_read_unlock();

	ret = resctrl_task_write_permission(tsk, of);
	if (!ret)
		ret = __resctrl_move_task(tsk, rg);

	put_task_struct(tsk);
	return ret;
}

static ssize_t tasks_write(char *buf, size_t nbytes, struct resctrl_group *rg,
			   struct kernfs_open_file *of)
{
	pid_t pid;

	if (kstrtoint(strstrip(buf), 0, &pid) || pid < 0)
		return -EINVAL;

	resctrl_last_cmd_clear();

	return resctrl_move_task(pid, rg, of);
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
	cfi->write = tasks_write;

	return true;
}

void resctrl_remove_task_file(struct kernfs_node *parent_kn, struct list_head *h)
{
	resctrl_remove_file("tasks", parent_kn, h);
}
