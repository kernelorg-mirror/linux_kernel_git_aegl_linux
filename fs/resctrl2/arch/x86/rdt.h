/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright(c) 2023 Intel Corporation. */

#define RESCTRL_FILE_DEF(X, fmt)					\
static int X##_show(struct seq_file *sf)				\
{									\
	seq_printf(sf, fmt, X);						\
	return 0;							\
}
