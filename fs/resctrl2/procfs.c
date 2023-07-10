// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2023 Intel Corporation. */

#include "internal.h"

#ifdef CONFIG_PROC_CPU_RESCTRL

/*
 * A task can only be part of one resctrl control group and of one monitor
 * group which is associated to that control group.
 *
 * 1)   res:
 *      mon:
 *
 *    resctrl is not available.
 *
 * 2)   res:/
 *      mon:
 *
 *    Task is part of the root resctrl control group, and it is not associated
 *    to any monitor group.
 *
 * 3)  res:/
 *     mon:mon0
 *
 *    Task is part of the root resctrl control group and monitor group mon0.
 *
 * 4)  res:group0
 *     mon:
 *
 *    Task is part of resctrl control group group0, and it is not associated
 *    to any monitor group.
 *
 * 5) res:group0
 *    mon:mon1
 *
 *    Task is part of resctrl control group group0 and monitor group mon1.
 */
int proc_resctrl_show(struct seq_file *s, struct pid_namespace *ns,
		      struct pid *pid, struct task_struct *tsk)
{
	struct resctrl_node_info *rni;
	struct resctrl_group *rg;
	int ret = 0;

	mutex_lock(&resctrl_mutex);

	/* Return empty if resctrl has not been mounted. */
	if (!resctrl_is_mounted) {
		seq_puts(s, "res:\nmon:\n");
		goto unlock;
	}

	list_for_each_entry(rg, &all_ctrl_groups, list) {
		struct resctrl_group *crg;

		/*
		 * Task information is only relevant for shareable
		 * and exclusive groups.
		 */
		if (rg->mode != RESCTRL_SHARED &&
		    rg->mode != RESCTRL_EXCLUSIVE)
			continue;

		if (!arch_match_control_id(tsk, rg))
			continue;

		rni = (struct resctrl_node_info *)rg - 1;
		seq_printf(s, "res:%s%s\n", (rg == resctrl_default) ? "/" : "",
			   rni->kn->name);
		seq_puts(s, "mon:");
		list_for_each_entry(crg, &rg->child_list, list) {
			if (tsk->resctrl_ids != crg->resctrl_ids)
				continue;
			rni = (struct resctrl_node_info *)crg - 1;
			seq_printf(s, "%s", rni->kn->name);
			break;
		}
		seq_putc(s, '\n');
		goto unlock;
	}
	/*
	 * The above search should succeed. Otherwise return
	 * with an error.
	 */
	ret = -ENOENT;
unlock:
	mutex_unlock(&resctrl_mutex);

	return ret;
}
#endif
