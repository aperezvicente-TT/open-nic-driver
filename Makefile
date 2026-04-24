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
BASE_OBJS := $(patsubst $(srcdir)/%.c,%.o,$(wildcard $(srcdir)/*.c $(srcdir)/*/*.c $(srcdir)/*/*/*.c))
onic-objs = $(BASE_OBJS)
ccflags-y = -O3 -Wall -Werror -I$(srcdir)/qdma_access -I$(srcdir)/hwmon -I$(srcdir)

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

KDIR ?= /lib/modules/$(KERNEL_VERS)/build

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

with-clang:
	$(MAKE) CC=clang -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f *.o.ur-safe
	rm -f ./qdma_access/*.o.ur-safe

install:
	rm -f /lib/modules/$(KERNEL_VERS)/onic.ko
	cp onic.ko /lib/modules/$(KERNEL_VERS)
	depmod

uninstall:
	rm -f /lib/modules/$(KERNEL_VERS)/onic.ko
	depmod
