// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2023 Intel Corporation. */

#include "internal.h"

LIST_HEAD(resctrl_all_resources);

void resctrl_activate(struct resctrl_resource *r)
{
	if (r->infodir)
		resctrl_addinfofiles(r);
}

int resctrl_register_resource(struct resctrl_resource *r)
{
	cpus_read_lock();
	mutex_lock(&resctrl_mutex);

	if (resctrl_is_mounted)
		resctrl_activate(r);

	list_add(&r->list, &resctrl_all_resources);

	mutex_unlock(&resctrl_mutex);
	cpus_read_unlock();

	return 0;
}
EXPORT_SYMBOL_GPL(resctrl_register_resource);

void resctrl_deactivate(struct resctrl_resource *r, struct list_head *h)
{
	if (r->infodir)
		resctrl_delinfofiles(r, h);
}

void resctrl_unregister_resource(struct resctrl_resource *r)
{
	LIST_HEAD(clean_list);

	cpus_read_lock();
	mutex_lock(&resctrl_mutex);

	if (resctrl_is_mounted)
		resctrl_deactivate(r, &clean_list);

	list_del(&r->list);

	mutex_unlock(&resctrl_mutex);
	cpus_read_unlock();

	resctrl_node_file_cleanup(&clean_list);
}
EXPORT_SYMBOL_GPL(resctrl_unregister_resource);
