// SPDX-License-Identifier: GPL-2.0-only
/*
 * Resource Director Technology(RDT)
 * - Intel Application Energy Telemetry
 *
 * Copyright (C) 2025 Intel Corporation
 *
 * Author:
 *    Tony Luck <tony.luck@intel.com>
 */

#define pr_fmt(fmt)   "resctrl: " fmt

#include <linux/cleanup.h>
#include <linux/cpu.h>
#include <linux/intel_vsec.h>
#include <linux/resctrl.h>

#include "internal.h"

/**
 * struct event_group - All information about a group of telemetry events.
 * @pfg:		Points to the aggregated telemetry space information
 *			within the OOBMSM driver that contains data for all
 *			telemetry regions.
 * @guid:		Unique number per XML description file.
 */
struct event_group {
	/* Data fields for additional structures to manage this group. */
	struct pmt_feature_group	*pfg;

	/* Remaining fields initialized from XML file. */
	u32				guid;
};

/*
 * Link: https://github.com/intel/Intel-PMT
 * File: xml/CWF/OOBMSM/RMID-ENERGY/cwf_aggregator.xml
 */
static struct event_group energy_0x26696143 = {
	.guid		= 0x26696143,
};

/*
 * Link: https://github.com/intel/Intel-PMT
 * File: xml/CWF/OOBMSM/RMID-PERF/cwf_aggregator.xml
 */
static struct event_group perf_0x26557651 = {
	.guid		= 0x26557651,
};

static struct event_group *known_event_groups[] = {
	&energy_0x26696143,
	&perf_0x26557651,
};

#define NUM_KNOWN_GROUPS ARRAY_SIZE(known_event_groups)

/* Stub for now */
static int configure_events(struct event_group *e, struct pmt_feature_group *p)
{
	return -EINVAL;
}

DEFINE_FREE(intel_pmt_put_feature_group, struct pmt_feature_group *,
		if (!IS_ERR_OR_NULL(_T))
			intel_pmt_put_feature_group(_T))

/*
 * Make a request to the INTEL_PMT_DISCOVERY driver for the
 * pmt_feature_group for a specific feature. If there is
 * one the returned structure has an array of telemetry_region
 * structures. Each describes one telemetry aggregator.
 * Try to configure any with a known matching guid.
 */
static bool get_pmt_feature(enum pmt_feature_id feature)
{
	struct pmt_feature_group *p __free(intel_pmt_put_feature_group) = NULL;
	struct event_group **peg;
	bool ret;

	p = intel_pmt_get_regions_by_feature(feature);

	if (IS_ERR_OR_NULL(p))
		return false;

	for (peg = &known_event_groups[0]; peg < &known_event_groups[NUM_KNOWN_GROUPS]; peg++) {
		ret = configure_events(*peg, p);
		if (!ret) {
			(*peg)->pfg = no_free_ptr(p);
			return true;
		}
	}

	return false;
}

/*
 * Ask OOBMSM discovery driver for all the RMID based telemetry groups
 * that it supports.
 */
bool intel_aet_get_events(void)
{
	bool ret1, ret2;

	ret1 = get_pmt_feature(FEATURE_PER_RMID_ENERGY_TELEM);
	ret2 = get_pmt_feature(FEATURE_PER_RMID_PERF_TELEM);

	return ret1 || ret2;
}

void __exit intel_aet_exit(void)
{
	struct event_group **peg;

	for (peg = &known_event_groups[0]; peg < &known_event_groups[NUM_KNOWN_GROUPS]; peg++) {
		if ((*peg)->pfg) {
			intel_pmt_put_feature_group((*peg)->pfg);
			(*peg)->pfg = NULL;
		}
	}
}
