/*
 * Copyright (c) 2020 Xilinx, Inc.
 * All rights reserved.
 *
 * This source code is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * The full GNU General Public License is included in this distribution in
 * the file called "COPYING".
 */
#ifndef __ONIC_LIB_H__
#define __ONIC_LIB_H__

#include "onic.h"

/**
 * onic_init_capacity - calculate the number of vectors and queues
 * @priv: pointer to driver private data
 *
 * Return 0 on success, negative on failure
 **/
int onic_init_capacity(struct onic_private *priv);

/**
 * onic_init_capacity_slave - mirror primary's capacity to a slave netdev
 * @slave: secondary (CMAC1) priv, shares MSI-X pool with @primary
 * @primary: primary (CMAC0) priv that already allocated MSI-X vectors
 *
 * The master PF's MSI-X allocation reserves twice the per-CMAC vectors so the
 * slave can claim the upper half via vec_base.  This helper propagates the
 * vector and queue counts without re-calling pci_alloc_irq_vectors.
 **/
void onic_init_capacity_slave(struct onic_private *slave,
			      struct onic_private *primary);

/**
 * onic_clear_capacity - reset the number of vectors and queues to zero
 * @priv: pointer to driver private data
 **/
void onic_clear_capacity(struct onic_private *priv);

/**
 * onic_init_interrupt - initialize interrupt resource
 * @priv: pointer to driver private data
 *
 * Return 0 on success, negative on failure
 **/
int onic_init_interrupt(struct onic_private *priv);

/**
 * onic_clear_interrupt - clear resource for all vectors
 * @priv: pointer to driver private data
 **/
void onic_clear_interrupt(struct onic_private *priv);

/**
 * onic_init_link_recovery - initialise the link-recovery work struct
 * @priv: pointer to driver private data
 **/
void onic_init_link_recovery(struct onic_private *priv);

/**
 * onic_init_error_rearm - initialise the error-interrupt deferred re-arm work
 * @priv: pointer to driver private data
 **/
void onic_init_error_rearm(struct onic_private *priv);

/**
 * onic_start_link_watchdog - start periodic link-status polling
 * @priv: pointer to driver private data
 **/
void onic_start_link_watchdog(struct onic_private *priv);

/**
 * onic_stop_link_watchdog - cancel the periodic link-status poll
 * @priv: pointer to driver private data
 **/
void onic_stop_link_watchdog(struct onic_private *priv);

#endif
