/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright(c) 2023 Intel Corporation. */

/* H/W supported RDT monitor events */
#define EV_LLC		1
#define EV_TOT		2
#define EV_LOC		3
#define EV_MAX		4

/* S/W events */
#define EV_TOTRATE	4
#define EV_LOCRATE	5

#define RESCTRL_FILE_DEF(X, fmt)					\
static int X##_show(struct seq_file *sf, struct resctrl_resource *r)	\
{									\
	seq_printf(sf, fmt, X);						\
	return 0;							\
}

u64 rdt_rmid_read(int domain_id, int rmid, int event);

#ifdef PSEUDO_LOCK
int pseudo_lock_cpu_check(void);
void pseudo_lock_release(void);
int pseudo_lock_create(struct resctrl_plr *plr);
void pseudo_lock_remove(struct resctrl_plr *plr);
#else
static inline int pseudo_lock_cpu_check(void) { return 0; }
static inline void pseudo_lock_release(void) { }
#endif
