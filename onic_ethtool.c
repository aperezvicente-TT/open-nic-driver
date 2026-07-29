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
#include <linux/if_link.h>
#include <linux/pci.h>
#include <linux/netdevice.h>
#include <linux/ethtool.h>
#include <linux/version.h>
#include <linux/ptp_clock_kernel.h>
#include <linux/net_tstamp.h>

#include "onic.h"
#include "onic_register.h"
#include "onic_netdev.h"

extern const char onic_drv_name[];
extern const char onic_drv_ver[];
void onic_set_ethtool_ops(struct net_device *netdev);
// netdev stats are stats kept by the driver, like xdp stats, onic_stats are kept in the NIC and accessed via the on-board registers 
enum { NETDEV_STATS, ONIC_STATS };

enum {
	ETHTOOL_XDP_REDIRECT,
	ETHTOOL_XDP_PASS,
	ETHTOOL_XDP_DROP,
	ETHTOOL_XDP_TX,
	ETHTOOL_XDP_TX_ERR,
	ETHTOOL_XDP_XMIT,
	ETHTOOL_XDP_XMIT_ERR,
	ETHTOOL_TX_DROPPED,
	ETHTOOL_TX_ERRORS,
};


struct onic_stats {
    char stat_string[ETH_GSTRING_LEN];
    int type;
    int sizeof_stat;
    int stat0_offset;
    int stat1_offset;
};

#define _STAT_ONIC(_name, _stat0, _stat1) { \
	.stat_string = _name, \
	.type = ONIC_STATS, \
	.sizeof_stat = sizeof(u32), \
	.stat0_offset = _stat0, \
	.stat1_offset = _stat1, \
}


#define _STAT_NETDEV(_name,_stat) {\
	.stat_string = _name, \
	.type = NETDEV_STATS, \
	.sizeof_stat = sizeof(u64), \
	.stat0_offset = _stat, \
	.stat1_offset = _stat, \
}

static const struct onic_stats onic_gstrings_stats[] = {
    _STAT_ONIC("stat_tx_total_pkts",
          CMAC_OFFSET_STAT_TX_TOTAL_PKTS(0),
          CMAC_OFFSET_STAT_TX_TOTAL_PKTS(1)),
    _STAT_ONIC("stat_tx_total_good_pkts",
          CMAC_OFFSET_STAT_TX_TOTAL_GOOD_PKTS(0),
          CMAC_OFFSET_STAT_TX_TOTAL_GOOD_PKTS(1)),
    _STAT_ONIC("stat_tx_total_bytes",
          CMAC_OFFSET_STAT_TX_TOTAL_BYTES(0),
          CMAC_OFFSET_STAT_TX_TOTAL_BYTES(1)),
    _STAT_ONIC("stat_tx_total_good_bytes",
          CMAC_OFFSET_STAT_TX_TOTAL_GOOD_BYTES(0),
          CMAC_OFFSET_STAT_TX_TOTAL_GOOD_BYTES(1)),
    _STAT_ONIC("stat_tx_pkt_64_bytes",
          CMAC_OFFSET_STAT_TX_PKT_64_BYTES(0),
          CMAC_OFFSET_STAT_TX_PKT_64_BYTES(1)),
    _STAT_ONIC("stat_tx_pkt_65_127_bytes",
          CMAC_OFFSET_STAT_TX_PKT_65_127_BYTES(0),
          CMAC_OFFSET_STAT_TX_PKT_65_127_BYTES(1)),
    _STAT_ONIC("stat_tx_pkt_128_255_bytes",
          CMAC_OFFSET_STAT_TX_PKT_128_255_BYTES(0),
          CMAC_OFFSET_STAT_TX_PKT_128_255_BYTES(1)),
    _STAT_ONIC("stat_tx_pkt_256_511_bytes",
          CMAC_OFFSET_STAT_TX_PKT_256_511_BYTES(0),
          CMAC_OFFSET_STAT_TX_PKT_256_511_BYTES(1)),
    _STAT_ONIC("stat_tx_pkt_512_1023_bytes",
          CMAC_OFFSET_STAT_TX_PKT_512_1023_BYTES(0),
          CMAC_OFFSET_STAT_TX_PKT_512_1023_BYTES(1)),
    _STAT_ONIC("stat_tx_pkt_1024_1518_bytes",
          CMAC_OFFSET_STAT_TX_PKT_1024_1518_BYTES(0),
          CMAC_OFFSET_STAT_TX_PKT_1024_1518_BYTES(1)),
    _STAT_ONIC("stat_tx_pkt_1519_1522_bytes",
          CMAC_OFFSET_STAT_TX_PKT_1519_1522_BYTES(0),
          CMAC_OFFSET_STAT_TX_PKT_1519_1522_BYTES(1)),
    _STAT_ONIC("stat_tx_pkt_1523_1548_bytes",
          CMAC_OFFSET_STAT_TX_PKT_1523_1548_BYTES(0),
          CMAC_OFFSET_STAT_TX_PKT_1523_1548_BYTES(1)),
    _STAT_ONIC("stat_tx_pkt_1549_2047_bytes",
          CMAC_OFFSET_STAT_TX_PKT_1549_2047_BYTES(0),
          CMAC_OFFSET_STAT_TX_PKT_1549_2047_BYTES(1)),
    _STAT_ONIC("stat_tx_pkt_2048_4095_bytes",
          CMAC_OFFSET_STAT_TX_PKT_2048_4095_BYTES(0),
          CMAC_OFFSET_STAT_TX_PKT_2048_4095_BYTES(1)),
    _STAT_ONIC("stat_tx_pkt_4096_8191_bytes",
          CMAC_OFFSET_STAT_TX_PKT_4096_8191_BYTES(0),
          CMAC_OFFSET_STAT_TX_PKT_4096_8191_BYTES(1)),
    _STAT_ONIC("stat_tx_pkt_8192_9215_bytes",
          CMAC_OFFSET_STAT_TX_PKT_8192_9215_BYTES(0),
          CMAC_OFFSET_STAT_TX_PKT_8192_9215_BYTES(1)),
    _STAT_ONIC("stat_tx_pkt_large",
          CMAC_OFFSET_STAT_TX_PKT_LARGE(0),
          CMAC_OFFSET_STAT_TX_PKT_LARGE(1)),
    _STAT_ONIC("stat_tx_pkt_small",
          CMAC_OFFSET_STAT_TX_PKT_SMALL(0),
          CMAC_OFFSET_STAT_TX_PKT_SMALL(1)),
    _STAT_ONIC("stat_tx_bad_fcs",
          CMAC_OFFSET_STAT_TX_BAD_FCS(0),
          CMAC_OFFSET_STAT_TX_BAD_FCS(1)),
    _STAT_ONIC("stat_tx_unicast",
          CMAC_OFFSET_STAT_TX_UNICAST(0),
          CMAC_OFFSET_STAT_TX_UNICAST(1)),
    _STAT_ONIC("stat_tx_multicast",
          CMAC_OFFSET_STAT_TX_MULTICAST(0),
          CMAC_OFFSET_STAT_TX_MULTICAST(1)),
    _STAT_ONIC("stat_tx_broadcast",
          CMAC_OFFSET_STAT_TX_BROADCAST(0),
          CMAC_OFFSET_STAT_TX_BROADCAST(1)),
    _STAT_ONIC("stat_tx_vlan",
          CMAC_OFFSET_STAT_TX_VLAN(0),
          CMAC_OFFSET_STAT_TX_VLAN(1)),
    _STAT_ONIC("stat_tx_pause",
          CMAC_OFFSET_STAT_TX_PAUSE(0),
          CMAC_OFFSET_STAT_TX_PAUSE(1)),
    _STAT_ONIC("stat_tx_user_pause",
          CMAC_OFFSET_STAT_TX_USER_PAUSE(0),
          CMAC_OFFSET_STAT_TX_USER_PAUSE(1)),
    _STAT_ONIC("stat_rx_total_pkts",
          CMAC_OFFSET_STAT_RX_TOTAL_PKTS(0),
          CMAC_OFFSET_STAT_RX_TOTAL_PKTS(1)),
    _STAT_ONIC("stat_rx_total_good_pkts",
          CMAC_OFFSET_STAT_RX_TOTAL_GOOD_PKTS(0),
          CMAC_OFFSET_STAT_RX_TOTAL_GOOD_PKTS(1)),
    _STAT_ONIC("stat_rx_total_bytes",
          CMAC_OFFSET_STAT_RX_TOTAL_BYTES(0),
          CMAC_OFFSET_STAT_RX_TOTAL_BYTES(1)),
    _STAT_ONIC("stat_rx_total_good_bytes",
          CMAC_OFFSET_STAT_RX_TOTAL_GOOD_BYTES(0),
          CMAC_OFFSET_STAT_RX_TOTAL_GOOD_BYTES(1)),
    _STAT_ONIC("stat_rx_pkt_64_bytes",
          CMAC_OFFSET_STAT_RX_PKT_64_BYTES(0),
          CMAC_OFFSET_STAT_RX_PKT_64_BYTES(1)),
    _STAT_ONIC("stat_rx_pkt_65_127_bytes",
          CMAC_OFFSET_STAT_RX_PKT_65_127_BYTES(0),
          CMAC_OFFSET_STAT_RX_PKT_65_127_BYTES(1)),
    _STAT_ONIC("stat_rx_pkt_128_255_bytes",
          CMAC_OFFSET_STAT_RX_PKT_128_255_BYTES(0),
          CMAC_OFFSET_STAT_RX_PKT_128_255_BYTES(1)),
    _STAT_ONIC("stat_rx_pkt_256_511_bytes",
          CMAC_OFFSET_STAT_RX_PKT_256_511_BYTES(0),
          CMAC_OFFSET_STAT_RX_PKT_256_511_BYTES(1)),
    _STAT_ONIC("stat_rx_pkt_512_1023_bytes",
          CMAC_OFFSET_STAT_RX_PKT_512_1023_BYTES(0),
          CMAC_OFFSET_STAT_RX_PKT_512_1023_BYTES(1)),
    _STAT_ONIC("stat_rx_pkt_1024_1518_bytes",
          CMAC_OFFSET_STAT_RX_PKT_1024_1518_BYTES(0),
          CMAC_OFFSET_STAT_RX_PKT_1024_1518_BYTES(1)),
    _STAT_ONIC("stat_rx_pkt_1519_1522_bytes",
          CMAC_OFFSET_STAT_RX_PKT_1519_1522_BYTES(0),
          CMAC_OFFSET_STAT_RX_PKT_1519_1522_BYTES(1)),
    _STAT_ONIC("stat_rx_pkt_1523_1548_bytes",
          CMAC_OFFSET_STAT_RX_PKT_1523_1548_BYTES(0),
          CMAC_OFFSET_STAT_RX_PKT_1523_1548_BYTES(1)),
    _STAT_ONIC("stat_rx_pkt_1549_2047_bytes",
          CMAC_OFFSET_STAT_RX_PKT_1549_2047_BYTES(0),
          CMAC_OFFSET_STAT_RX_PKT_1549_2047_BYTES(1)),
    _STAT_ONIC("stat_rx_pkt_2048_4095_bytes",
          CMAC_OFFSET_STAT_RX_PKT_2048_4095_BYTES(0),
          CMAC_OFFSET_STAT_RX_PKT_2048_4095_BYTES(1)),
    _STAT_ONIC("stat_rx_pkt_4096_8191_bytes",
           CMAC_OFFSET_STAT_RX_PKT_4096_8191_BYTES(0),
           CMAC_OFFSET_STAT_RX_PKT_4096_8191_BYTES(1)),
    _STAT_ONIC("stat_rx_pkt_8192_9215_bytes",
           CMAC_OFFSET_STAT_RX_PKT_8192_9215_BYTES(0),
           CMAC_OFFSET_STAT_RX_PKT_8192_9215_BYTES(1)),
    _STAT_ONIC("stat_rx_pkt_large",
           CMAC_OFFSET_STAT_RX_PKT_LARGE(0),
           CMAC_OFFSET_STAT_RX_PKT_LARGE(1)),
    _STAT_ONIC("stat_rx_pkt_small",
           CMAC_OFFSET_STAT_RX_PKT_SMALL(0),
           CMAC_OFFSET_STAT_RX_PKT_SMALL(1)),
    _STAT_ONIC("stat_rx_undersize",
           CMAC_OFFSET_STAT_RX_UNDERSIZE(0),
           CMAC_OFFSET_STAT_RX_UNDERSIZE(1)),
    _STAT_ONIC("stat_rx_fragment",
           CMAC_OFFSET_STAT_RX_FRAGMENT(0),
           CMAC_OFFSET_STAT_RX_FRAGMENT(1)),
    _STAT_ONIC("stat_rx_oversize",
           CMAC_OFFSET_STAT_RX_OVERSIZE(0),
           CMAC_OFFSET_STAT_RX_OVERSIZE(1)),
    _STAT_ONIC("stat_rx_toolong",
          CMAC_OFFSET_STAT_RX_TOOLONG(0),
          CMAC_OFFSET_STAT_RX_TOOLONG(1)),
    _STAT_ONIC("stat_rx_jabber",
          CMAC_OFFSET_STAT_RX_JABBER(0),
          CMAC_OFFSET_STAT_RX_JABBER(1)),
    _STAT_ONIC("stat_rx_bad_fcs",
           CMAC_OFFSET_STAT_RX_BAD_FCS(0),
           CMAC_OFFSET_STAT_RX_BAD_FCS(1)),
    _STAT_ONIC("stat_rx_pkt_bad_fcs",
          CMAC_OFFSET_STAT_RX_PKT_BAD_FCS(0),
          CMAC_OFFSET_STAT_RX_PKT_BAD_FCS(1)),
    _STAT_ONIC("stat_rx_stomped_fcs",
           CMAC_OFFSET_STAT_RX_STOMPED_FCS(0),
           CMAC_OFFSET_STAT_RX_STOMPED_FCS(1)),
    _STAT_ONIC("stat_rx_unicast",
          CMAC_OFFSET_STAT_RX_UNICAST(0),
          CMAC_OFFSET_STAT_RX_UNICAST(1)),
    _STAT_ONIC("stat_rx_multicast",
          CMAC_OFFSET_STAT_RX_MULTICAST(0),
          CMAC_OFFSET_STAT_RX_MULTICAST(1)),
    _STAT_ONIC("stat_rx_broadcast",
          CMAC_OFFSET_STAT_RX_BROADCAST(0),
          CMAC_OFFSET_STAT_RX_BROADCAST(1)),
    _STAT_ONIC("stat_rx_vlan",
          CMAC_OFFSET_STAT_RX_VLAN(0),
          CMAC_OFFSET_STAT_RX_VLAN(1)),
    _STAT_ONIC("stat_rx_pause",
          CMAC_OFFSET_STAT_RX_PAUSE(0),
          CMAC_OFFSET_STAT_RX_PAUSE(1)),
    _STAT_ONIC("stat_rx_user_pause",
          CMAC_OFFSET_STAT_RX_USER_PAUSE(0),
          CMAC_OFFSET_STAT_RX_USER_PAUSE(1)),
    _STAT_ONIC("stat_rx_inrangeerr",
          CMAC_OFFSET_STAT_RX_INRANGEERR(0),
          CMAC_OFFSET_STAT_RX_INRANGEERR(1)),
    _STAT_ONIC("stat_rx_truncated",
          CMAC_OFFSET_STAT_RX_TRUNCATED(0),
          CMAC_OFFSET_STAT_RX_TRUNCATED(1)),
    _STAT_ONIC("stat_adapt_tx_sent",
          CMAC_ADPT_OFFSET_TX_PKT_RECV(0),
          CMAC_ADPT_OFFSET_TX_PKT_RECV(1)),
    _STAT_ONIC("stat_adapt_tx_drop",
          CMAC_ADPT_OFFSET_TX_PKT_DROP(0),
          CMAC_ADPT_OFFSET_TX_PKT_DROP(1)),
    _STAT_ONIC("stat_adapt_rx_recv",
          CMAC_ADPT_OFFSET_RX_PKT_RECV(0),
          CMAC_ADPT_OFFSET_RX_PKT_RECV(1)),
    _STAT_ONIC("stat_adapt_rx_drop",
          CMAC_ADPT_OFFSET_RX_PKT_DROP(0),
          CMAC_ADPT_OFFSET_RX_PKT_DROP(1)),
    _STAT_ONIC("stat_adapt_rx_error",
          CMAC_ADPT_OFFSET_RX_PKT_ERROR(0),
          CMAC_ADPT_OFFSET_RX_PKT_ERROR(1)),



          
      
    _STAT_NETDEV("rx_xdp_redirect", ETHTOOL_XDP_REDIRECT),
    _STAT_NETDEV("rx_xdp_pass", ETHTOOL_XDP_PASS ),
    _STAT_NETDEV("rx_xdp_drop", ETHTOOL_XDP_DROP ),
    _STAT_NETDEV("rx_xdp_tx",ETHTOOL_XDP_TX ),
    _STAT_NETDEV("rx_xdp_tx_errors", ETHTOOL_XDP_TX_ERR ),
    _STAT_NETDEV("tx_xdp_xmit", ETHTOOL_XDP_XMIT ),
    _STAT_NETDEV("tx_xdp_xmit_errors", ETHTOOL_XDP_XMIT_ERR ),
    _STAT_NETDEV("tx_dropped", ETHTOOL_TX_DROPPED),
    _STAT_NETDEV("tx_errors", ETHTOOL_TX_ERRORS),
};

#define ONIC_QUEUE_STATS_LEN 0
#define ONIC_GLOBAL_STATS_LEN ARRAY_SIZE(onic_gstrings_stats)
#define ONIC_STATS_LEN        (ONIC_GLOBAL_STATS_LEN + ONIC_QUEUE_STATS_LEN)

static void onic_get_drvinfo(struct net_device *netdev,
			     struct ethtool_drvinfo *drvinfo)
{
	struct onic_private *priv = netdev_priv(netdev);

	strscpy(drvinfo->driver, onic_drv_name, sizeof(drvinfo->driver));
	strscpy(drvinfo->version, onic_drv_ver,
		sizeof(drvinfo->version));
	strscpy(drvinfo->bus_info, pci_name(priv->pdev),
		sizeof(drvinfo->bus_info));
}

static u32 onic_get_link(struct net_device *netdev)
{
    u32 val;
    u8 cmac_idx;
    struct onic_private *priv = netdev_priv(netdev);
    struct onic_hardware *hw = &priv->hw;

    cmac_idx = priv->cmac_id;

    /* read twice to flush any previously latched value */
    val = onic_read_reg(hw, CMAC_OFFSET_STAT_RX_STATUS(cmac_idx));
    val = onic_read_reg(hw, CMAC_OFFSET_STAT_RX_STATUS(cmac_idx));

    val = (val == 0x3);

    /* Sync carrier to hardware reality.  If the FPGA link was already up
     * when the driver loaded, no link-change IRQ fires and carrier is
     * never set by the IRQ path.  Update it here from ground truth.
     * Only assert on — the IRQ path owns the carrier-off transition. */
    if (val)
        netif_carrier_on(netdev);

    onic_netdev_dbg(ONIC_DBG_INFO, netdev,
		    "get_link port: %d rx_status_ok: %u", cmac_idx, val);

    return val;
}

static int onic_get_fecparam(struct net_device *netdev,
			     struct ethtool_fecparam *fec)
{
    struct onic_private *priv = netdev_priv(netdev);
    struct onic_hardware *hw = &priv->hw;
    u8 cmac_idx = priv->cmac_id;
    u32 rsfec_en;

    fec->fec = ETHTOOL_FEC_RS | ETHTOOL_FEC_OFF;

    rsfec_en = onic_read_reg(hw, CMAC_OFFSET_RSFEC_CONF_ENABLE(cmac_idx));
    fec->active_fec = (rsfec_en & 0x3) ? ETHTOOL_FEC_RS : ETHTOOL_FEC_OFF;

    return 0;
}

static int onic_set_fecparam(struct net_device *netdev,
			     struct ethtool_fecparam *fec)
{
    struct onic_private *priv = netdev_priv(netdev);
    struct onic_hardware *hw = &priv->hw;
    u8 cmac_idx = priv->cmac_id;

    if (fec->fec & ETHTOOL_FEC_RS) {
	onic_write_reg(hw, CMAC_OFFSET_RSFEC_CONF_ENABLE(cmac_idx), 0x3);
	onic_write_reg(hw, CMAC_OFFSET_RSFEC_CONF_IND_CORRECTION(cmac_idx), 0x7);
	priv->RS_FEC = 1;
	hw->RS_FEC = 1;
    } else if (fec->fec & ETHTOOL_FEC_OFF) {
	onic_write_reg(hw, CMAC_OFFSET_RSFEC_CONF_ENABLE(cmac_idx), 0x0);
	onic_write_reg(hw, CMAC_OFFSET_RSFEC_CONF_IND_CORRECTION(cmac_idx), 0x0);
	priv->RS_FEC = 0;
	hw->RS_FEC = 0;
    } else {
	return -EINVAL;
    }

    return 0;
}

static int onic_get_link_ksettings(struct net_device *netdev,
				   struct ethtool_link_ksettings *cmd)
{
    struct onic_private *priv = netdev_priv(netdev);
    struct onic_hardware *hw = &priv->hw;
    u8 cmac_idx = priv->cmac_id;
    u32 rx_status;

    ethtool_link_ksettings_zero_link_mode(cmd, supported);
    ethtool_link_ksettings_zero_link_mode(cmd, advertising);

    ethtool_link_ksettings_add_link_mode(cmd, supported, 100000baseCR4_Full);
    ethtool_link_ksettings_add_link_mode(cmd, supported, 100000baseSR4_Full);
    ethtool_link_ksettings_add_link_mode(cmd, supported, 100000baseLR4_ER4_Full);
    ethtool_link_ksettings_add_link_mode(cmd, supported, 100000baseKR4_Full);
    ethtool_link_ksettings_add_link_mode(cmd, supported, FEC_RS);
    ethtool_link_ksettings_add_link_mode(cmd, supported, FEC_NONE);

    ethtool_link_ksettings_add_link_mode(cmd, advertising, 100000baseCR4_Full);
    ethtool_link_ksettings_add_link_mode(cmd, advertising, 100000baseSR4_Full);
    ethtool_link_ksettings_add_link_mode(cmd, advertising, 100000baseLR4_ER4_Full);
    ethtool_link_ksettings_add_link_mode(cmd, advertising, 100000baseKR4_Full);

    if (hw->RS_FEC)
	ethtool_link_ksettings_add_link_mode(cmd, advertising, FEC_RS);
    else
	ethtool_link_ksettings_add_link_mode(cmd, advertising, FEC_NONE);

    rx_status = onic_read_reg(hw, CMAC_OFFSET_STAT_RX_STATUS(cmac_idx));
    rx_status = onic_read_reg(hw, CMAC_OFFSET_STAT_RX_STATUS(cmac_idx));

    if (rx_status == 0x3) {
	cmd->base.speed = SPEED_100000;
	cmd->base.duplex = DUPLEX_FULL;
    } else {
	cmd->base.speed = SPEED_UNKNOWN;
	cmd->base.duplex = DUPLEX_UNKNOWN;
    }

    cmd->base.port = PORT_DA;
    cmd->base.autoneg = AUTONEG_DISABLE;

    return 0;
}

static void onic_get_ethtool_stats(struct net_device *netdev,
            struct ethtool_stats /*__always_unused*/ *stats,
            u64 *data)
{
    struct onic_private *priv = netdev_priv(netdev);
    struct onic_hardware *hw = &priv->hw;
    int i,j;
    u16 func_id;
    u32 off;

    struct {
            u64 xdp_redirect;
            u64 xdp_pass;
            u64 xdp_drop;
            u64 xdp_tx;
            u64 xdp_tx_err;
            u64 xdp_xmit;
            u64 xdp_xmit_err;
      } global_xdp_stats = {0};
    u64 tx_dropped = 0, tx_errors = 0;
    struct rtnl_link_stats64 *pcpu_ptr;
    unsigned int cpu;


      for (j =0; j < priv->num_rx_queues; j++) {
        global_xdp_stats.xdp_redirect += priv->rx_queue[j]->xdp_rx_stats.xdp_redirect;
        global_xdp_stats.xdp_pass += priv->rx_queue[j]->xdp_rx_stats.xdp_pass;
        global_xdp_stats.xdp_drop += priv->rx_queue[j]->xdp_rx_stats.xdp_drop;
        global_xdp_stats.xdp_tx += priv->rx_queue[j]->xdp_rx_stats.xdp_tx;
        global_xdp_stats.xdp_tx_err += priv->rx_queue[j]->xdp_rx_stats.xdp_tx_err;

      }
      for (j =0; j < priv->num_tx_queues; j++) {
        global_xdp_stats.xdp_xmit += priv->tx_queue[j]->xdp_tx_stats.xdp_xmit;
        global_xdp_stats.xdp_xmit_err += priv->tx_queue[j]->xdp_tx_stats.xdp_xmit_err;
      }
      for_each_possible_cpu(cpu) {
        pcpu_ptr = per_cpu_ptr(priv->netdev_stats, cpu);
        tx_dropped += pcpu_ptr->tx_dropped;
        tx_errors += pcpu_ptr->tx_errors;
      }
    
    func_id = priv->cmac_id;

    // Note :
    //   write 1 into REG_TICK (offset 0x2B0).
    //   this is WriteOnce/SelfClear (WO/SC).
    //   with this, the cmac system updates all STAT_* registers.
    onic_write_reg(hw, CMAC_OFFSET_TICK(func_id), 1);

    for (i = 0; i < ONIC_GLOBAL_STATS_LEN; i++) {
      if (onic_gstrings_stats[i].type == ONIC_STATS) {
        if (func_id == 0)
          off = onic_gstrings_stats[i].stat0_offset;
        else
          off = onic_gstrings_stats[i].stat1_offset;
        data[i] = onic_read_reg(hw, off);
      } else {
        switch (onic_gstrings_stats[i].stat0_offset) {

        case ETHTOOL_XDP_REDIRECT:
          data[i] = global_xdp_stats.xdp_redirect;
          break;
        case ETHTOOL_XDP_PASS:
          data[i] = global_xdp_stats.xdp_pass;
          break;
        case ETHTOOL_XDP_DROP:
          data[i] = global_xdp_stats.xdp_drop;
          break;
        case ETHTOOL_XDP_TX:
          data[i] = global_xdp_stats.xdp_tx;
          break;
        case ETHTOOL_XDP_TX_ERR:
          data[i] = global_xdp_stats.xdp_tx_err;
          break;
        case ETHTOOL_XDP_XMIT:
          data[i] = global_xdp_stats.xdp_xmit;
          break;
        case ETHTOOL_XDP_XMIT_ERR:
          data[i] = global_xdp_stats.xdp_xmit_err;
          break;
        case ETHTOOL_TX_DROPPED:
          data[i] = tx_dropped;
          break;
        case ETHTOOL_TX_ERRORS:
          data[i] = tx_errors;
          break;
        }
      }
    }

    return;
}

static void onic_get_strings(struct net_device *netdev, u32 stringset,
			      u8 *data)
{
	u8 *p = data;
	int i;

    for (i = 0; i < ONIC_GLOBAL_STATS_LEN; i++) {
        memcpy(p, onic_gstrings_stats[i].stat_string,
            ETH_GSTRING_LEN);
        p += ETH_GSTRING_LEN;
    }
}

static int onic_get_sset_count(struct net_device *netdev, int sset)
{
    return ONIC_STATS_LEN;
}

static u32 onic_get_rxfh_indir_size(struct net_device *dev)
{
	return INDIRECTION_TABLE_SIZE;
}

static u32 onic_get_rxfh_key_size(struct net_device *netdev)
{
	return ONIC_EN_RSS_KEY_SIZE;
}


static int onic_get_rxfh(struct net_device *dev, u32 *ring_index, u8 *key,
		u8 *hfunc)
{
	struct onic_private *priv = netdev_priv(dev);
	u32 n = onic_get_rxfh_indir_size(dev);
      u16 func_id = priv->cmac_id;
	u32 i;

     	if (ring_index) {
		for (i = 0; i < n; i++) {
			ring_index[i] = 0xFFFF & onic_read_reg(&priv->hw, QDMA_FUNC_OFFSET_INDIR_TABLE(func_id,i));
			
		}
	}

	if (key) {
		for (i = 0; i < ONIC_EN_RSS_KEY_SIZE/4; i++) {
			u32 val = onic_read_reg(&priv->hw, QDMA_FUNC_OFFSET_HASH_KEY(func_id,i));
			memcpy(&key[i*4],&val, 4);
		}
	}

	if (hfunc)
		*hfunc = ETH_RSS_HASH_TOP;

	return 0;
}



static int onic_set_rxfh(struct net_device *dev, const u32 *ring_index,
			    const u8 *key, const u8 hfunc)
{

	
	struct onic_private *priv = netdev_priv(dev);
	int n = onic_get_rxfh_indir_size(dev);
  u16 func_id = priv->cmac_id;
	int i=0;
      
	if (hfunc != ETH_RSS_HASH_NO_CHANGE && hfunc != ETH_RSS_HASH_TOP)
		return -EOPNOTSUPP;

	if (ring_index) {
            for (i = 0; i < n; i++) {
                  // Check that the ring index is within the number of rx queues
                  if (ring_index[i] >= priv->num_rx_queues) {
                        printk("error in onic_set_rxfh: ring_index >= priv->num_rx_queues\n");
                        return -EINVAL;
                  }
                  onic_write_reg(&priv->hw, QDMA_FUNC_OFFSET_INDIR_TABLE(func_id,i), ring_index[i]);
            }
      }

	if (key) {
	      for (i = 0; i < ONIC_EN_RSS_KEY_SIZE/4; i++) {
                  u32 val;
                  memcpy(&val, &key[i*4], 4);
                  onic_write_reg(&priv->hw, QDMA_FUNC_OFFSET_HASH_KEY(func_id,i), val);
	      }
      }
	return 0;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 8, 0)
static int onic_set_rxfh_new(struct net_device *dev,
                             struct ethtool_rxfh_param *rxfh_param,
                             struct netlink_ext_ack *extack) {
  return onic_set_rxfh(dev, rxfh_param->indir, rxfh_param->key,
                       rxfh_param->hfunc);
}

static int onic_get_rxfh_new(struct net_device *dev,
                             struct ethtool_rxfh_param *rxfh) {
  return onic_get_rxfh(dev, rxfh->indir, rxfh->key, &rxfh->hfunc);
}
#endif

static int onic_get_rxnfc(struct net_device *dev, struct ethtool_rxnfc *info, u32 *rule_locs) {
	struct onic_private *priv = netdev_priv(dev);
	
      switch (info->cmd) {
	case ETHTOOL_GRXRINGS:
		info->data =  priv->num_rx_queues;
		return 0;
	case ETHTOOL_GRXFH:
		return -EOPNOTSUPP;
	default:
		return -EOPNOTSUPP;
	}
}


static u32 onic_get_msglevel(struct net_device *netdev)
{
    struct onic_private *priv = netdev_priv(netdev);
    return priv->msg_enable;
}

static void onic_set_msglevel(struct net_device *netdev, u32 val)
{
    struct onic_private *priv = netdev_priv(netdev);
    priv->msg_enable = val;
}

/*
 * kernel_ethtool_ts_info was introduced in v6.10 (commit that renamed
 * ethtool_ts_info -> kernel_ethtool_ts_info).  Before that the callback
 * takes a plain struct ethtool_ts_info *.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 10, 0)
static int onic_get_ts_info(struct net_device *dev,
			    struct kernel_ethtool_ts_info *info)
#else
static int onic_get_ts_info(struct net_device *dev,
			    struct ethtool_ts_info *info)
#endif
{
	struct onic_private *priv = netdev_priv(dev);

	info->so_timestamping = SOF_TIMESTAMPING_TX_HARDWARE |
				SOF_TIMESTAMPING_RX_HARDWARE |
				SOF_TIMESTAMPING_RAW_HARDWARE |
				SOF_TIMESTAMPING_TX_SOFTWARE |
				SOF_TIMESTAMPING_RX_SOFTWARE |
				SOF_TIMESTAMPING_SOFTWARE;
	info->tx_types = BIT(HWTSTAMP_TX_OFF) | BIT(HWTSTAMP_TX_ON);
	info->rx_filters = BIT(HWTSTAMP_FILTER_NONE) | BIT(HWTSTAMP_FILTER_ALL);

	if (priv->ptp_clock)
		info->phc_index = ptp_clock_index(priv->ptp_clock);
	else
		info->phc_index = -1;

	return 0;
}

/*
 * Whether set_coalesce re-initialises the queues to apply a change.
 *
 * Needed today because the governing thresholds live in the per-queue completion
 * context (see onic_set_coalesce).  Adaptive moderation (DIM, shell docs Ch. 12)
 * changes profiles every few milliseconds and cannot bounce queues, so it needs a
 * path that applies a change in place.  Setting this to 0 exercises exactly that
 * path -- the counter/timer fields carried by each CMPT CIDX update, with no
 * re-init -- which is how Ch. 12 Step 1 determines whether such a path exists.
 *
 * Leave at 1 for correct behaviour; 0 is a measurement aid.
 */
static bool coalesce_bounce = true;
module_param(coalesce_bounce, bool, 0644);
MODULE_PARM_DESC(coalesce_bounce,
	"re-init queues on ethtool -C so the change actually applies (default true; "
	"0 = CIDX-only path, for measuring whether in-place application works)");

/*
 * C2H completion coalescing.  RX only -- the H2C side has no equivalent
 * completion-interrupt moderation in this design.
 *
 * Both values are rounded to the nearest entry of the fixed QDMA pools
 * (cnt_th = 2,4,8,...,192 entries; tmr_cnt = 1,2,4,...,200 ticks of
 * C2H_INT_TIMER_TICK), so a read-back after a write will often differ from
 * what was asked for.  Measured effect of moving off the historical
 * 2-frames/1-tick default: single-port RX 36.8 -> 98.5 Gbit/s.
 */
static int onic_get_coalesce(struct net_device *netdev,
			     struct ethtool_coalesce *ec,
			     struct kernel_ethtool_coalesce *kec,
			     struct netlink_ext_ack *extack)
{
	struct onic_private *priv = netdev_priv(netdev);
	u32 frames = 0, usecs = 0;

	onic_qdma_get_coalesce(priv->hw.qdma, &frames, &usecs);
	ec->rx_max_coalesced_frames = frames;
	ec->rx_coalesce_usecs = usecs;
	return 0;
}

static int onic_set_coalesce(struct net_device *netdev,
			     struct ethtool_coalesce *ec,
			     struct kernel_ethtool_coalesce *kec,
			     struct netlink_ext_ack *extack)
{
	struct onic_private *priv = netdev_priv(netdev);
	int rv;

	rv = onic_qdma_set_coalesce(priv->hw.qdma, ec->rx_max_coalesced_frames,
				    ec->rx_coalesce_usecs);
	if (rv < 0)
		return rv;

	/*
	 * The thresholds that actually govern interrupt generation live in the
	 * per-queue completion CONTEXT, written when the queue is initialised.
	 * The counter/timer fields carried by each CMPT CIDX update do not
	 * override it: measured, setting 64 frames / 3 us live left RX at
	 * 35.0 Gbit/s, while the identical values after a queue re-init gave
	 * 98.5 Gbit/s.  So bounce the queues, as other drivers do for coalesce
	 * and ring changes.  Costs a brief link flap.
	 */
	if (netif_running(netdev) && coalesce_bounce) {
		rv = onic_stop_netdev(netdev);
		if (rv < 0)
			netdev_warn(netdev, "coalesce: stop failed (%d)\n", rv);
		rv = onic_open_netdev(netdev);
		if (rv < 0) {
			netdev_err(netdev,
				   "coalesce: re-open failed (%d); interface is down\n",
				   rv);
			return rv;
		}
	}

	/* Report what the hardware will actually use, so the rounding is visible
	 * in the very next `ethtool -c`. */
	onic_qdma_get_coalesce(priv->hw.qdma, &ec->rx_max_coalesced_frames,
			       &ec->rx_coalesce_usecs);
	netdev_info(netdev, "coalesce: rx-frames %u rx-usecs %u (tick %u ns)\n",
		    ec->rx_max_coalesced_frames, ec->rx_coalesce_usecs,
		    onic_qdma_cmpl_tick_ns(priv->hw.qdma));
	return 0;
}

static const struct ethtool_ops onic_ethtool_ops = {
    .supported_coalesce_params = ETHTOOL_COALESCE_RX_USECS |
				 ETHTOOL_COALESCE_RX_MAX_FRAMES,
    .get_coalesce        = onic_get_coalesce,
    .set_coalesce        = onic_set_coalesce,
    .get_drvinfo         = onic_get_drvinfo,
    .get_msglevel        = onic_get_msglevel,
    .set_msglevel        = onic_set_msglevel,
    .get_link            = onic_get_link,
    .get_link_ksettings  = onic_get_link_ksettings,
    .get_fecparam        = onic_get_fecparam,
    .set_fecparam        = onic_set_fecparam,
    .get_ethtool_stats   = onic_get_ethtool_stats,
    .get_strings         = onic_get_strings,
    .get_sset_count      = onic_get_sset_count,
    .get_rxfh_indir_size = onic_get_rxfh_indir_size,
    .get_rxfh_key_size   = onic_get_rxfh_key_size,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 8, 0)
    .set_rxfh            = onic_set_rxfh_new,
    .get_rxfh            = onic_get_rxfh_new,
#else
    .get_rxfh            = onic_get_rxfh,
    .set_rxfh            = onic_set_rxfh,
#endif
    .get_rxnfc           = onic_get_rxnfc,
    .get_ts_info         = onic_get_ts_info,
};

void onic_set_ethtool_ops(struct net_device *netdev)
{
    netdev->ethtool_ops = &onic_ethtool_ops;
}
