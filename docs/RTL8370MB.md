# RTL8370MB Switch — UDM Pro Hardware Reference

## Status: WORKING (2026-03-27)

Traffic flows through all 8 LAN ports via the RTL8370MB to the SoC.
Flat L2 bridge, no VLANs yet. VLANs will be managed by sfgw.

## Hardware Topology

```
                eth1 (WAN RJ45)               eth3 (Switch Uplink)
                PCI 00:01.0                    PCI 00:03.0
                     │                              │
              ┌──────┴──────┐               ┌───────┴───────┐
              │  1G MAC 1   │               │   1G MAC 3    │
              │  AR8031 PHY │               │  RGMII 1000M  │
              │  MDIO + Data│               │  phy_exist=no  │
              └──────┬──────┘               └───────┬───────┘
                     │                              │
          MDIO Bus (shared)                   RGMII Data Path
            ┌────────┴────────┐                     │
            │                 │                     │
       AR8031 PHY      RTL8370MB              RTL8370MB
       Addr 0x04       Addr 0x1D             CPU Port 1
       (WAN PHY)       (SMI Mgmt)            (EXT1, Data)
```

- SMI management: via eth1's MDIO bus at PHY address **0x1D (29)**
- Data path: via eth3's RGMII to the switch's EXT1 (CPU port 1)
- Security note: switch management shares the WAN RJ45 MDIO bus

## Init Sequence

### 1. GPIO Reset (boot)

PCA9575 GPIO expander (I2C bus 0, addr 0x28) pins 4 and 8 must be
driven HIGH to release the switch from hardware reset.

OpenRC service `switch-gpio` runs at boot before networking:

```bash
echo 0-0028 > /sys/bus/i2c/drivers/pca953x/unbind
i2cset -y 0 0x28 0x02 0x10    # Output Port 0: Pin 4 HIGH
i2cset -y 0 0x28 0x03 0x01    # Output Port 1: Pin 8 HIGH
i2cset -y 0 0x28 0x06 0xef    # Config Port 0: Pin 4 output
i2cset -y 0 0x28 0x07 0xfe    # Config Port 1: Pin 8 output
echo 0-0028 > /sys/bus/i2c/drivers/pca953x/bind
```

Note: mainline `gpio-hog` DTS feature does NOT work with this
pca953x driver version. I2C writes are the only reliable method.

### 2. al_eth Driver (eth3 open)

The al_eth driver reads board params from PCI config space:
`phy_exist=Yes, phy_addr=17, media_type=1 (RGMII)`

For port 3 (phy_addr == 0x11), the driver overrides:
- `phy_exist = false` — prevents 5+ minute MDIO timeout
- `active_speed = 1000, active_duplex = 1` — fixed link
- MAC mode stays RGMII (correct for this hardware)

With `phy_exist=false`, `al_eth_up` calls `al_eth_mac_link_config`
directly instead of waiting for a PHY driver. This is the critical
step — without it, the RGMII MAC never gets speed/duplex configured
and RX silently fails (0 bytes received).

### 3. rtl8370mb Kernel Module (switch init)

Loaded via `/etc/modules` after `al_eth`. Communicates via SMI
protocol over the shared MDIO bus (eth1 must be UP first).

Configures:
- EXT1 (port 9): RGMII mode, forced 1000M/FD/link-up
- Port isolation: all LAN ports (0-7) ↔ CPU port (9)
- STP state: forwarding on all active ports
- MAC learning: enabled, limit 2112
- CPU port: EXT1, no tagging (transparent L2 bridge)
- Max frame size: 16383 bytes (jumbo)

## SMI Protocol

```
MDIO PHY Address: 0x1D (29) — Realtek default, hardcoded

Write register:
  MDIO write addr=0x1D reg=31 val=0x000E    # Address mode
  MDIO write addr=0x1D reg=23 val=<reg>     # Target register
  MDIO write addr=0x1D reg=24 val=<data>    # Data to write
  MDIO write addr=0x1D reg=21 val=0x0003    # Write operation

Read register:
  MDIO write addr=0x1D reg=31 val=0x000E    # Address mode
  MDIO write addr=0x1D reg=23 val=<reg>     # Target register
  MDIO write addr=0x1D reg=21 val=0x0001    # Read operation
  MDIO read  addr=0x1D reg=25 → data        # Read result

Unlock: SMI write reg=0x13C2 val=0x0249
Lock:   SMI write reg=0x13C2 val=0x0000
```

## Internal PHY Access (for LAN ports 0-7)

```
IA_CTRL  = 0x1F00  (command register)
IA_STAT  = 0x1F01  (status/busy)
IA_ADDR  = 0x1F02  (address register)
IA_WDATA = 0x1F03  (write data)
IA_RDATA = 0x1F04  (read data)

PHY reg address = 0x2000 + (port * 32) + register
Internal PHY ID: 001c:c982 (Realtek GbE PHY)
```

## Chip Info

| Field     | Value  |
|-----------|--------|
| Chip ID   | 0x6368 |
| Chip Ver  | 0x0010 |
| PHY ports | 0-7 (8x GbE RJ45) |
| EXT0      | Port 8 (disabled) |
| EXT1      | Port 9 (CPU uplink) |
| EXT2      | Port 10 (unused) |

## Key Findings & Gotchas

1. **MDIO address 0x1D, NOT 0x11**: The DTS `reg = <0x11>` is an
   internal device ID. The actual SMI PHY address is 0x1D (29),
   hardcoded in the Realtek vendor driver.

2. **SMI goes through eth1, NOT eth3**: The shared MDIO bus is on
   eth1's MAC controller. eth3 has a separate MDIO controller that
   goes nowhere.

3. **phy_exist must be false for eth3**: Board params say `phy_exist=Yes`
   but there's no standard PHY at addr 17. Setting `phy_exist=false`
   makes the driver call `link_config` directly — without this, RGMII
   RX silently fails (packets arrive with 0 bytes).

4. **GPIO pins 4+8 on PCA9575**: Must be HIGH before switch responds
   on MDIO. Without this, SMI reads return 0x0000.

5. **Stock firmware port renaming**: Stock kernel renames PCI 00:03.0
   to eth0 (not eth3). Our kernel keeps the PCI probe order.

## Stock Firmware Reference

- Firmware: `dff0-UDMPRO-5.0.16`
- Kernel: 4.19.152-ui-alpine
- Switch driver: `drivers/net/phy/rtl8370.c` (built-in)
- Uses `swconfig` for runtime VLAN/port configuration
- Board-cfg DTS says `sgmii-2.5g` but actual MAC mode is RGMII
  (board-cfg is consumed by stock driver's separate init path)
