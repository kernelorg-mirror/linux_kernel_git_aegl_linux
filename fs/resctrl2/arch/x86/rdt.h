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

#define RESCTRL_FILE_DEF(X, fmt)			\
static int X##_show(struct seq_file *sf, void *v)	\
{							\
	seq_printf(sf, fmt, X);				\
	return 0;					\
}							\
static struct kernfs_ops X##_ops = {			\
	.seq_show	= X##_show			\
};
