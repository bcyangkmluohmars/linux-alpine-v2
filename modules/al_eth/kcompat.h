/*
 * kcompat.h - Kernel 6.12 LTS compatibility header for al_eth driver
 *
 * This is a minimal kcompat header targeting ONLY Linux 6.12 LTS.
 * All backwards compatibility code for older kernels has been removed.
 *
 * Copyright (c) 2016 Amazon.com, Inc.
 * Copyright (c) 2024 - Ported to kernel 6.12
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation.
 */

#ifndef _KCOMPAT_H_
#define _KCOMPAT_H_

#include <linux/version.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/ioport.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/delay.h>
#include <linux/sched.h>
#include <linux/in.h>
#include <linux/ip.h>
#include <linux/udp.h>
#include <linux/mii.h>
#include <linux/vmalloc.h>
#include <asm/io.h>
#include <linux/ethtool.h>
#include <linux/phy.h>
#include <linux/if_vlan.h>
#include <linux/u64_stats_sync.h>
#include <generated/utsrelease.h>

#define adapter_struct al_eth_adapter
#define adapter_q_vector al_eth_napi

/* NAPI is always available in 6.12 */
#define NAPI

/* net_device_ops always available in 6.12 */
#define HAVE_NET_DEVICE_OPS

/* ndo_set_features always available in 6.12 */
#define HAVE_NDO_SET_FEATURES

/* set_rx_mode always available in 6.12 */
#define HAVE_SET_RX_MODE

/* NETDEV napi list always available in 6.12 */
#define HAVE_NETDEV_NAPI_LIST

/* VLAN hardware acceleration helper - always use CTAG variant in 6.12 */
static inline int al_eth_vlan_hwaccel_check_and_put(netdev_features_t *features,
						struct sk_buff *skb,
						struct vlan_ethhdr *veh) {
	if ((*features & NETIF_F_HW_VLAN_CTAG_RX) == NETIF_F_HW_VLAN_CTAG_RX &&
		veh->h_vlan_proto == htons(ETH_P_8021Q)) {
		__vlan_hwaccel_put_tag(skb, htons(ETH_P_8021Q), ntohs(veh->h_vlan_TCI));
		return 0;
	}
	return -EOPNOTSUPP;
}

#endif /* _KCOMPAT_H_ */
