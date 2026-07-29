# 
# Copyright (c) 2020 Xilinx, Inc.
# All rights reserved.
# 
# This source code is free software; you can redistribute it and/or modify it
# under the terms and conditions of the GNU General Public License,
# version 2, as published by the Free Software Foundation.
# 
# This program is distributed in the hope that it will be useful, but WITHOUT
# ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
# FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
# more details.
# 
# The full GNU General Public License is included in this distribution in
# the file called "COPYING".
#
ifdef KVERSION
KERNEL_VERS = $(KVERSION)
else
KERNEL_VERS = $(shell uname -r)
endif

srcdir = $(PWD)
obj-m += onic.o

SRC_FOLDERS = . qdma_legacy hwmon \
              libqdma libqdma/qdma_access \
              libqdma/qdma_access/eqdma_soft_access \
              libqdma/qdma_access/eqdma_cpm5_access \
              libqdma/qdma_access/qdma_soft_access \
              libqdma/qdma_access/qdma_cpm4_access \
              libqdma/qdma_access/qdma_s80_hard_access

onic-objs := $(foreach D,$(SRC_FOLDERS),$(patsubst $(srcdir)/%.c,%.o,$(wildcard $(srcdir)/$(D)/*.c)))

# Dead RDMA/ERNIC sources.  This branch is pure Ethernet: the call sites were
# stripped (dda0ea7) but the files are still on disk, and the wildcard above
# would compile them -- which (a) drags in MLNX_OFED because they reference
# ib_* symbols, and (b) fails outright, since onic_ib.c includes onic_debugfs.h
# which has never existed in this tree.  Kept on disk for reference only.
#
# Set BUILD_RDMA=1 to build them again (requires onic_debugfs.h and OFED).
BUILD_RDMA ?= 0
RDMA_SRCS := onic_ib.c onic_ernic_irq.c onic_ddr_alloc.c
ifneq ($(BUILD_RDMA),1)
  RDMA_OBJS := $(RDMA_SRCS:.c=.o)
  onic-objs := $(filter-out $(RDMA_OBJS) $(addprefix ./,$(RDMA_OBJS)),$(onic-objs))
endif

ccflags-y = -O3 -Wall -I$(srcdir)/qdma_legacy -I$(srcdir)/hwmon -I$(srcdir) \
            $(foreach D,$(SRC_FOLDERS),-I$(srcdir)/$(D))
ccflags-y += -DMBOX_INTERRUPT_DISABLE

# MLNX_OFED integration: if OFED is installed, build against its rdma/*
# headers and link CRCs against OFED's per-kernel ib_core Module.symvers.
# Without this, ib_register_device / ib_alloc_device etc. fail at insmod
# with "disagrees about version of symbol".
#
# IMPORTANT: do NOT trust /usr/src/ofa_kernel/default — the alternatives
# symlink may point to a stale kernel (observed pointing to 6.8.0-40-generic
# even when the running kernel is -110), with mismatched CRCs.  Prefer the
# per-kernel tree at /usr/src/ofa_kernel/x86_64/<kernel>/.
OFA_DIR ?= /usr/src/ofa_kernel/x86_64/$(KERNEL_VERS)
ifeq ($(wildcard $(OFA_DIR)/Module.symvers),)
  # Fallback: alternatives path (may be stale — warn the user)
  OFA_DIR := /usr/src/ofa_kernel/default
  ifneq ($(wildcard $(OFA_DIR)/Module.symvers),)
    $(warning Using $(OFA_DIR) fallback; verify CRCs match running ib_core!)
  endif
endif

ifeq ($(BUILD_RDMA),1)
ifneq ($(wildcard $(OFA_DIR)/Module.symvers),)
  KBUILD_EXTRA_SYMBOLS := $(OFA_DIR)/Module.symvers
  export KBUILD_EXTRA_SYMBOLS
  # Prepend OFED includes to LINUXINCLUDE so <rdma/ib_verbs.h> resolves
  # to OFED's larger struct layout BEFORE the kernel's stock header.
  # ccflags-y alone isn't enough — its -I comes AFTER kernel's LINUXINCLUDE.
  override LINUXINCLUDE := -I$(OFA_DIR)/include -I$(OFA_DIR)/include/uapi $(LINUXINCLUDE)
  export LINUXINCLUDE
  $(info Building against MLNX_OFED at $(OFA_DIR))

  # OFED 2601+ extended reg_user_mr and create_cq op signatures.  Feature-
  # detect so the driver compiles on older OFED too.
  ifneq ($(shell grep -c 'struct ib_dmah \*dmah' $(OFA_DIR)/include/rdma/ib_verbs.h 2>/dev/null),0)
    ccflags-y += -DOFED_HAVE_IB_DMAH
  endif
endif
else
  $(info Pure-Ethernet build: RDMA sources excluded, no MLNX_OFED dependency)
endif

KDIR ?= /lib/modules/$(KERNEL_VERS)/build

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

with-clang:
	$(MAKE) CC=clang -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f *.o.ur-safe
	$(foreach D,$(SRC_FOLDERS),rm -f $(srcdir)/$(D)/*.o.ur-safe;)

install:
	rm -f /lib/modules/$(KERNEL_VERS)/onic.ko
	cp onic.ko /lib/modules/$(KERNEL_VERS)
	depmod

uninstall:
	rm -f /lib/modules/$(KERNEL_VERS)/onic.ko
	depmod
