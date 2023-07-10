# SPDX-License-Identifier: GPL-2.0

obj-$(CONFIG_X86_CPU_RESCTRL2) += alloc.o
obj-$(CONFIG_X86_CPU_RESCTRL2) += rdt_monitor.o
obj-$(CONFIG_X86_CPU_RESCTRL2) += rdt_mbm_adjust.o

CFLAGS_rdt_l3_cat.o += -DCACHE_LEVEL=3
CFLAGS_rdt_l2_cat.o += -DCACHE_LEVEL=2
CFLAGS_rdt_l3_cdp.o += -DCACHE_LEVEL=3 -DCDP
CFLAGS_rdt_l2_cdp.o += -DCACHE_LEVEL=2 -DCDP

obj-$(CONFIG_X86_RDT_L3_CAT)		+= rdt_l3_cat.o
obj-$(CONFIG_X86_RDT_L2_CAT)		+= rdt_l2_cat.o
obj-$(CONFIG_X86_RDT_L3_CDP)		+= rdt_l3_cdp.o
obj-$(CONFIG_X86_RDT_L2_CDP)		+= rdt_l2_cdp.o

CFLAGS_rdt_llc_occupancy.o		+= -DEVENT=1
CFLAGS_rdt_mbm_total_bytes.o		+= -DEVENT=2
CFLAGS_rdt_mbm_local_bytes.o		+= -DEVENT=3
CFLAGS_rdt_mbm_total_rate.o		+= -DEVENT=4
CFLAGS_rdt_mbm_local_rate.o		+= -DEVENT=5

obj-$(CONFIG_X86_RDT_LLC_OCCUPANCY)	+= rdt_llc_occupancy.o
obj-$(CONFIG_X86_RDT_MBM_TOTAL_BYTES)	+= rdt_mbm_total_bytes.o
obj-$(CONFIG_X86_RDT_MBM_LOCAL_BYTES)	+= rdt_mbm_local_bytes.o
obj-$(CONFIG_X86_RDT_MBM_TOTAL_RATE)	+= rdt_mbm_total_rate.o
obj-$(CONFIG_X86_RDT_MBM_LOCAL_RATE)	+= rdt_mbm_local_rate.o

obj-$(CONFIG_X86_RDT_L3_MBA)		+= rdt_l3_mba.o
obj-$(CONFIG_X86_RDT_L3_MBA_MBPS)	+= rdt_l3_mba_MBps.o
