# Kernel Build Matrix — Alpine V2 Targets

## Build Targets

| | UNVR (NAS) | UDM Pro (GW) |
|---|:---:|:---:|
| **Defconfig** | `unvr_defconfig` | `udmpro_defconfig` |
| **Device Tree** | `alpine-v2-ubnt-unvr.dts` | `alpine-v2-ubnt-udmpro.dts` |
| **Board ID** | ea1a | ea15 |
| **Use Case** | secfirstNAS | secfirstGW |

## Feature Matrix

| Feature | UNVR (NAS) | UDM Pro (GW) |
|---------|:----------:|:------------:|
| **SoC / Core** | | |
| Alpine V2 (4x Cortex-A57) | x | x |
| pcie-al-internal (SMCC snoop) | x | x |
| pcie-al DBI fix (external PCIe) | x | x |
| Alpine MSI-X | x | x |
| ARM64 Crypto (AES-CE, SHA-CE) | x | x |
| ZONE_DMA32 | x | x |
| **Storage** | | |
| AHCI SATA | x | x |
| MD RAID 0/1/5/10 | x | - |
| dm-crypt (LUKS) | x | - |
| dm-verity | x | - |
| Btrfs | x | - |
| **Networking** | | |
| al_eth (1GbE + 10GbE) | x | x |
| Bridge | module | builtin |
| 802.1Q VLAN | module | builtin |
| Bridge VLAN filtering | - | x |
| WireGuard VPN | - | x |
| **Netfilter / Firewall** | | |
| nftables (base) | x | x |
| NFT CT/LOG/LIMIT/REJECT | x | x |
| NFT NAT | x | x |
| NFT masquerade | x | x |
| NFT redirect | - | x |
| NFT counter / quota | - | x |
| NFT FIB (inet/v4/v6) | - | x |
| nftables bridge | - | x |
| iptables filter | x | x |
| iptables NAT + masquerade | x | x |
| iptables mangle | - | x |
| ip6tables filter | x | x |
| ip6tables NAT + masquerade | - | x |
| ip6tables mangle | - | x |
| Bridge netfilter | - | x |
| XT match: conntrack, state | x | x |
| XT match: mark, limit, multiport | - | x |
| XT match: comment, mac, iprange | - | x |
| XT target: LOG | x | x |
| XT target: MARK, CLASSIFY | - | x |
| Conntrack helpers (FTP/TFTP/SIP) | - | x |
| Conntrack mark / zones / events | - | x |
| **QoS / Traffic Shaping** | | |
| FQ_CODEL | x | x |
| HTB (Hierarchical Token Bucket) | - | x |
| TBF (Token Bucket Filter) | - | x |
| SFQ (Stochastic Fair Queuing) | - | x |
| PRIO (Priority Scheduler) | - | x |
| Ingress qdisc | - | x |
| IFB (Intermediate Functional Block) | - | x |
| cls_fw / cls_u32 classifiers | - | x |
| act_mirred / act_police | - | x |
| **I2C / GPIO / Sensors** | | |
| I2C DesignWare | x | x |
| I2C MUX PCA954x | x | x |
| GPIO PL061 | x | x |
| GPIO PCA953x (PCA9575) | x | x |
| GPIO DWAPB | x | x |
| SGPO (al_sgpo, serial GPIO) | x | x |
| ADT7475 HWMON (fan/temp) | x | x |
| LM63 HWMON (temp) | x | - |
| **LEDs** | | |
| GPIO LEDs | x | x |
| LED trigger: timer | x | x |
| LED trigger: heartbeat | x | x |
| LED trigger: default-on | x | x |
| LED trigger: disk activity | x | x |
| **Serial / Console** | | |
| 8250 UART (4 ports) | x | x |
| DesignWare 8250 | x | x |
| **USB** | | |
| xHCI (USB 3.0) | x | x |
| EHCI / OHCI | x | x |
| USB Storage | x | x |
| USB Serial (CP210x, FTDI) | x | x |
| USB ACM (LCM display) | - | x |
| **Boot / Flash** | | |
| SPI NOR (DesignWare SSI) | x | x |
| MTD partitions | x | x |
| **RTC** | | |
| DS1307 | x | x |
| S35390A | x | x |
| **Filesystems** | | |
| ext4 | x | x |
| Btrfs | x | - |
| SquashFS (zlib, zstd) | x | x |
| OverlayFS | x | x |
| VFAT | x | x |
| NFS client (v4.2) | x | x |
| NFS server (v4) | x | - |
| CIFS/SMB client | x | - |
| eCryptFS | x | - |
| FUSE | x | x |
| pstore / ramoops | x | x |
| **Crypto** | | |
| AES (ARM64 CE + NEON) | x | x |
| SHA-1/256/512 (ARM64 CE) | x | x |
| ChaCha20 (NEON) | x | x |
| Poly1305 (NEON) | x | x |
| GCM / CCM / XTS / CBC | x | x |
| RSA / ECDH | x | x |
| DRBG (HMAC) | x | x |
| **Security** | | |
| KASLR | x | x |
| Stack protector (strong) | x | x |
| VMAP stack | x | x |
| Hardened usercopy | x | x |
| FORTIFY_SOURCE | x | x |
| seccomp | x | x |
| LSM (landlock, lockdown, yama) | x | x |

## Out-of-Tree Modules

| Module | UNVR (NAS) | UDM Pro (GW) | Description |
|--------|:----------:|:------------:|-------------|
| `al_eth.ko` | x | x | 1GbE + 10GbE Ethernet + RTL8370 switch mgmt |
| `al_dma.ko` | x | x | RAID5/6 HW parity acceleration (XOR/PQ) |
| `al_ssm.ko` | x | x | HW AES-XTS/CBC crypto acceleration |
| `al_sgpo.ko` | x | x | Serial GPIO output (SATA activity LEDs) |
