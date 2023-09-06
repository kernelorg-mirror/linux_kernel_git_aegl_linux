// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2023 Intel Corporation. */

#include "internal.h"

/* Mutex to protect resctrl group access. */
DEFINE_MUTEX(resctrl_mutex);
