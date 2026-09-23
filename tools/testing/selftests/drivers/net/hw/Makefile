# SPDX-License-Identifier: GPL-2.0+ OR MIT

# Check if io_uring supports zero-copy receive
HAS_IOURING_ZCRX := $(shell \
	echo -e '#include <liburing.h>\n' \
	     'void *func = (void *)io_uring_register_ifq;\n' \
	     'int main() {return 0;}' | \
	$(CC) -luring -x c - -o /dev/null 2>&1 && echo y)

ifeq ($(HAS_IOURING_ZCRX),y)
COND_GEN_FILES += iou-zcrx
else
$(warning excluding iouring tests, liburing not installed or too old)
endif

XSK_NET_OUTPUT = $(abspath $(OUTPUT)/.xsk-net)
EXTRA_CLEAN += $(XSK_NET_OUTPUT)

# xsk.py may copy xskxceiver to a host with an older libc.  Prefer a
# static binary when the required archives are installed.  XSK_STATIC=0/1
# can be used to override the probe.
PKG_CONFIG ?= $(CROSS_COMPILE)pkg-config
XSK_STATIC_LIBS := $(shell $(PKG_CONFIG) libelf --static --libs 2>/dev/null || echo -lelf -lz -lzstd)
XSK_STATIC ?= $(shell echo 'int main(void) { return 0; }' | \
	$(CC) -x c - -static $(XSK_STATIC_LIBS) -lz -o /dev/null 2>/dev/null && echo 1)

BPFTOOL ?= bpftool
HAS_BPFTOOL := $(shell command -v $(BPFTOOL) >/dev/null 2>&1 && echo y)
ifeq ($(HAS_BPFTOOL),y)
COND_GEN_FILES += xskxceiver
else
$(warning excluding xskxceiver test, bpftool not found)
endif

TEST_GEN_FILES := \
	$(COND_GEN_FILES) \
# end of TEST_GEN_FILES

TEST_PROGS = \
	csum.py \
	devlink_rate_cross_esw.py \
	devlink_rate_tc_bw.py \
	devmem.py \
	ethtool.sh \
	ethtool_extended_state.sh \
	ethtool_mm.sh \
	ethtool_rmon.sh \
	ethtool_std_stats.sh \
	gro_stats.py \
	hw_stats_l3.sh \
	hw_stats_l3_gre.sh \
	iou-zcrx.py \
	ipsec_vxlan.py \
	irq.py \
	loopback.sh \
	nic_timestamp.py \
	nk_devmem.py \
	nk_netns.py \
	nk_qlease.py \
	ntuple.py \
	pp_alloc_fail.py \
	rss_api.py \
	rss_ctx.py \
	rss_drv.py \
	rss_flow_label.py \
	rss_input_xfrm.py \
	toeplitz.py \
	tso.py \
	userns_devmem.py \
	uso.py \
	xdp_metadata.py \
	xsk.py \
	xsk_reconfig.py \
	#

TEST_PROGS_EXTENDED := \
	devlink_port_split.py \
# end of TEST_PROGS_EXTENDED

TEST_FILES := \
	devmem_lib.py \
	ethtool_lib.sh \
	#

TEST_INCLUDES := \
	$(wildcard lib/py/*.py ../lib/py/*.py) \
	../../../net/lib.sh \
	../../../net/forwarding/ipip_lib.sh \
	../../../net/forwarding/lib.sh \
	../../../net/forwarding/tc_common.sh \
	#

# YNL files, must be before "include ..lib.mk"
YNL_GEN_FILES := \
	ncdevmem \
	toeplitz \
# end of YNL_GEN_FILES
TEST_GEN_FILES += $(YNL_GEN_FILES)
TEST_GEN_FILES += $(patsubst %.c,%.o,$(wildcard *.bpf.c))

include ../../../lib.mk

# YNL build
YNL_GENS := \
	ethtool \
	netdev \
# end of YNL_GENS

include ../../../net/ynl.mk

include ../../../net/bpf.mk

ifeq ($(HAS_IOURING_ZCRX),y)
$(OUTPUT)/iou-zcrx: LDLIBS += -luring
endif

# The generic AF_XDP suite and its build live in selftests/net.
$(OUTPUT)/xskxceiver: FORCE
	$(Q)mkdir -p $(XSK_NET_OUTPUT)
	+$(Q)$(MAKE) -C ../../../net OUTPUT=$(XSK_NET_OUTPUT) BPFTOOL=$(BPFTOOL) \
		XSK_STATIC=$(XSK_STATIC) $(XSK_NET_OUTPUT)/xskxceiver
	$(Q)cp $(XSK_NET_OUTPUT)/xskxceiver $@

.PHONY: FORCE xskxceiver
xskxceiver: $(OUTPUT)/xskxceiver

FORCE:
