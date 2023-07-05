/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_RESCTRL_H
#define _ASM_X86_RESCTRL_H

#ifdef CONFIG_X86_CPU_RESCTRL

#include <linux/sched.h>
#include <linux/jump_label.h>

/**
 * struct resctrl_pqr_state - State cache for the PQR MSR
 * @cur_rmid:		The cached Resource Monitoring ID
 * @cur_closid:	The cached Class Of Service ID
 * @default_rmid:	The user assigned Resource Monitoring ID
 * @default_closid:	The user assigned cached Class Of Service ID
 *
 * The upper 32 bits of MSR_IA32_PQR_ASSOC contain closid and the
 * lower 10 bits rmid. The update to MSR_IA32_PQR_ASSOC always
 * contains both parts, so we need to cache them. This also
 * stores the user configured per cpu CLOSID and RMID.
 *
 * The cache also helps to avoid pointless updates if the value does
 * not change.
 */
struct resctrl_pqr_state {
	u32			cur_rmid;
	u32			cur_closid;
	u32			default_rmid;
	u32			default_closid;
};

DECLARE_PER_CPU(struct resctrl_pqr_state, pqr_state);

DECLARE_STATIC_KEY_FALSE(rdt_enable_key);
DECLARE_STATIC_KEY_FALSE(rdt_alloc_enable_key);
DECLARE_STATIC_KEY_FALSE(rdt_mon_enable_key);

/*
 * __resctrl_sched_in() - Writes the task's CLOSid/RMID to IA32_PQR_MSR
 *
 * Following considerations are made so that this has minimal impact
 * on scheduler hot path:
 * - This will stay as no-op unless we are running on an Intel SKU
 *   which supports resource control or monitoring and we enable by
 *   mounting the resctrl file system.
 * - Caches the per cpu CLOSid/RMID values and does the MSR write only
 *   when a task with a different CLOSid/RMID is scheduled in.
 * - We allocate RMIDs/CLOSids globally in order to keep this as
 *   simple as possible.
 * Must be called with preemption disabled.
 */
static inline void __resctrl_sched_in(struct task_struct *tsk)
{
	struct resctrl_pqr_state *state = this_cpu_ptr(&pqr_state);
	u32 closid = state->default_closid;
	u32 rmid = state->default_rmid;
	u32 tmp;

	/*
	 * If this task has a closid/rmid assigned, use it.
	 * Else use the closid/rmid assigned to this cpu.
	 */
	if (static_branch_likely(&rdt_alloc_enable_key)) {
		tmp = READ_ONCE(tsk->closid);
		if (tmp)
			closid = tmp;
	}

	if (static_branch_likely(&rdt_mon_enable_key)) {
		tmp = READ_ONCE(tsk->rmid);
		if (tmp)
			rmid = tmp;
	}

	if (closid != state->cur_closid || rmid != state->cur_rmid) {
		state->cur_closid = closid;
		state->cur_rmid = rmid;
		wrmsr(MSR_IA32_PQR_ASSOC, rmid, closid);
	}
}

static inline unsigned int resctrl_arch_round_mon_val(unsigned int val)
{
	unsigned int scale = boot_cpu_data.x86_cache_occ_scale;

	/* h/w works in units of "boot_cpu_data.x86_cache_occ_scale" */
	val /= scale;
	return val * scale;
}

static inline void resctrl_sched_in(struct task_struct *tsk)
{
	if (static_branch_likely(&rdt_enable_key))
		__resctrl_sched_in(tsk);
}

void resctrl_cpu_detect(struct cpuinfo_x86 *c);

#elif defined(CONFIG_X86_CPU_RESCTRL2)

bool arch_alloc_resctrl_ids(struct resctrl_group *rg);
void arch_free_resctrl_ids(struct resctrl_group *rg);
bool arch_init_alloc_ids(struct resctrl_resource *r);
int rmid_alloc(int prmid);
void rmid_free(int rmid);
void arch_add_monitor(int mon_event);
void arch_del_monitor(int mon_event);
void rdt_mbm_apply_quirk(int num_rmids);
u64 get_corrected_mbm_count(u32 rmid, unsigned long val);

static inline bool is_closid_match(struct task_struct *t, struct resctrl_group *rg)
{
	return (t->resctrl_ids >> 32) == (rg->resctrl_ids >> 32);
}

static inline bool arch_is_resctrl_id_match(struct task_struct *t, struct resctrl_group *rg)
{
	if (rg->type == DIR_MON)
		return t->resctrl_ids == rg->resctrl_ids;
	return is_closid_match(t, rg);
}

static inline bool arch_match_control_id(struct task_struct *t, struct resctrl_group *rg)
{
	return is_closid_match(t, rg);
}

static inline bool arch_set_task_ids(struct task_struct *t, struct resctrl_group *rg)
{
	if (rg->type == DIR_MON) {
		if (!is_closid_match(t, rg)) {
			//rdt_last_cmd_puts("Can't move task to different control group\n");
			return false;
		}
	}

	WRITE_ONCE(t->resctrl_ids, rg->resctrl_ids);

	return true;
}
#else

static inline void resctrl_sched_in(struct task_struct *tsk) {}
static inline void resctrl_cpu_detect(struct cpuinfo_x86 *c) {}

#endif /* CONFIG_X86_CPU_RESCTRL */

#endif /* _ASM_X86_RESCTRL_H */
