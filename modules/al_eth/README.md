# al_eth - Annapurna Labs Ethernet Driver (Ported to Linux 6.12 LTS)

## Overview

Out-of-tree kernel module for the Annapurna Labs (Amazon) unified 1GbE/10GbE/25GbE
Ethernet controller found in Alpine V2 SoC (e.g., Ubiquiti UDM-Pro).

- **Module name:** `al_eth.ko`
- **PCI Vendor ID:** `0x1c36`
- **Target kernel:** Linux 6.12 LTS (out-of-tree build)
- **License:** GPL

## Building

```bash
# Native build (if running kernel 6.12)
make

# Cross-compile for aarch64 (Alpine V2 / UDM-Pro)
make KDIR=/path/to/linux-6.12-build ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
```

## Port from Linux 5.5 to 6.12 - Changes Made

### kcompat.h / kcompat.c — Complete Rewrite
- **Removed** ~500 lines of backwards-compatibility code targeting kernels 2.4 through 5.5
- kcompat.h now only defines feature-detection macros that are always true on 6.12:
  `HAVE_NET_DEVICE_OPS`, `HAVE_NDO_SET_FEATURES`, `HAVE_SET_RX_MODE`,
  `HAVE_NETDEV_NAPI_LIST`, `NAPI`
- kcompat.c is now empty (no compatibility shim functions needed)

### al_eth_main.c — API Updates

#### net_device_ops
- `ndo_do_ioctl` → `ndo_eth_ioctl` (renamed in 5.15 for ethernet-specific ioctls)
- Removed `#ifdef HAVE_NET_DEVICE_OPS` guards (always available since 2.6.29)
- Removed `#ifdef HAVE_SET_RX_MODE` guards around `ndo_set_rx_mode` and
  `al_eth_set_rx_mode` / `al_eth_mac_table_all_multicast_add`
- `ndo_change_mtu`: kept manual MTU validation but uses `WRITE_ONCE(dev->mtu, new_mtu)`
  (kernel 6.12 sets min_mtu/max_mtu on the netdev, stack does range checking)
- `ndo_get_stats64`: return type is already `void` (correct for 6.12)
- `ndo_select_queue`: signature already matches 6.12
  `(struct net_device *, struct sk_buff *, struct net_device *sb_dev)`

#### ethtool_ops
- `get_settings` / `set_settings` (deprecated `ethtool_cmd`) →
  `get_link_ksettings` / `set_link_ksettings` (uses `ethtool_link_ksettings`)
- `get_coalesce` / `set_coalesce`: added `struct kernel_ethtool_coalesce *`
  and `struct netlink_ext_ack *` parameters (added in 5.15)
- Added `supported_coalesce_params` field to `ethtool_ops` struct
- `get_rxfh` / `set_rxfh`: updated to use `struct ethtool_rxfh_param *`
  (API changed in 6.8, old `(indir, key, hfunc)` parameters replaced)
- Removed all `LINUX_VERSION_CODE` conditionals around ethtool functions
  (rxnfc, rxfh, channels, eee, wol)
- Enabled the `ethtool_ops` assignment (`netdev->ethtool_ops = &al_eth_ethtool_ops`)
  which was previously disabled with `#if 0`

#### String Functions
- `strlcpy()` → `strscpy()` (strlcpy deprecated in 6.8)

#### Statistics Sync
- `u64_stats_fetch_begin_irq()` → `u64_stats_fetch_begin()` (renamed in 6.3)
- `u64_stats_fetch_retry_irq()` → `u64_stats_fetch_retry()` (renamed in 6.3)

#### Memory Management
- `kzfree()` → `kfree_sensitive()` (renamed in 5.9)

#### RX Hash
- Removed old `skb->rxhash` / `skb->l4_rxhash` code path (removed from sk_buff
  long ago), keeping only the `skb_set_hash()` path
- Removed `#if defined(NETIF_F_RXHASH)` guards (always available)

#### Network Features
- Removed `#ifdef` guards around `NETIF_F_IPV6_CSUM`, `NETIF_F_RXHASH`,
  `NETIF_F_HW_VLAN_CTAG_RX`, `IFF_UNICAST_FLT` (all always available in 6.12)
- Removed `NETIF_F_MQ_TX_LOCK_OPT` references (not in mainline)
- Removed `#ifdef HAVE_NDO_SET_FEATURES` around `netdev->hw_features`

#### IRQ Affinity
- Removed `#ifdef HAVE_IRQ_AFFINITY_HINT` guards (always available)

#### MDIO Bus
- Removed `LINUX_VERSION_CODE` checks for mdio bus ID format and parent device

#### NAPI
- `netif_napi_add()` already uses 3-argument form (weight removed in 6.1,
  the driver was already updated for this)

### al_eth.h
- Removed `#ifndef BIT` guard (BIT() always available via kernel headers)
- Removed `#ifndef HAVE_NETDEV_NAPI_LIST` / `poll_dev` field from `al_eth_napi`
  struct (netdev napi list always available in 6.12)

### al_eth_sysfs.h / al_eth_sysfs.c
- Removed `LINUX_VERSION_CODE > KERNEL_VERSION(3,3,0)` guards
  (sysfs API stable since 3.3, we target 6.12)

### HAL Layer (al_hal_*.c/h)
- **No changes.** Pure register-level hardware abstraction code using standard
  `readl()`/`writel()` and kernel primitives that remain stable.

### Makefile
- Complete rewrite for out-of-tree `M=` build against kernel 6.12
- Supports `KDIR`, `ARCH`, and `CROSS_COMPILE` variables
- Includes all source objects needed for the module

## Hardware Support

| Board Type | PCI Device ID | Description |
|---|---|---|
| ALPINE_INTEGRATED | 0x0001, 0x0002 | Integrated SoC ethernet |
| ALPINE_NIC | (vendor-specific) | PCIe NIC mode, 1G/10G |
| ALPINE_NIC_V2_10 | (vendor-specific) | V2 10G PCIe NIC |
| ALPINE_NIC_V2_25 | (vendor-specific) | V2 25G PCIe NIC |
| ALPINE_NIC_V2_25_DUAL | (vendor-specific) | V2 dual-port 25G PCIe NIC |
