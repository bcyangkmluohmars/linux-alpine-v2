# Linux 6.12 LTS for Annapurna Labs Alpine V2

Mainline Linux 6.12 kernel port for the **Annapurna Labs Alpine V2 (AL-314)** SoC, used in:

- **Ubiquiti UNVR** (UniFi Network Video Recorder)
- **Ubiquiti UNAS** (UniFi NAS)
- **Ubiquiti UDM-Pro** (UniFi Dream Machine Pro)
- **QNAP TS-x32x** series NAS
- **MikroTik RB1100AHx4** (RouterBOARD)

## Why?

Amazon acquired Annapurna Labs in 2015 and pivoted to Graviton server chips. The Alpine V2 SoC was abandoned — critical drivers were never upstreamed to mainline Linux. Existing devices are stuck on kernel 4.19 (EOL).

This project provides everything needed to run a **modern Linux 6.12 LTS kernel** on Alpine V2 hardware.

## What's Included

### Kernel Patches (`patches/`)

| Patch | Description |
|-------|-------------|
| `pcie-al-internal.c` | Internal PCIe host controller with AXI SMCC snoop + APP_CONTROL configuration. Replaces the out-of-tree `annapurna-labs,alpine-internal-pcie` driver. |
| `pcie-al-dbi-fix.c` | DWC PCIe DBI base offset fix for external PCIe (xHCI/USB). |
| `quirks.c` (snippet) | PCI fixup for AXI snoop on all Annapurna Labs PCI devices. |

### Out-of-Tree Modules (`modules/`)

| Module | Description | Source |
|--------|-------------|--------|
| `al_eth` | 1GbE + 10GbE Ethernet driver with shared MDIO bus | Ported from [delroth/al_eth-standalone](https://github.com/delroth/al_eth-standalone) (5.5 → 6.12) |
| `al_dma` | RAID5/6 hardware parity acceleration (XOR/PQ) | HAL from [delroth/alpine_hal](https://github.com/delroth/alpine_hal) |
| `al_ssm` | Hardware AES-XTS/CBC crypto engine | HAL from alpine_hal |
| `al_sgpo` | Serial GPIO Output controller for HDD bay LEDs | Reverse-engineered from firmware |
| `rtl8370mb` | RTL8370MB 8-port GbE switch init (SMI-over-MDIO) | Reverse-engineered from stock UDM Pro firmware |

### Device Trees (`dts/`)

- `alpine-v2-ubnt-unvr.dts` — Ubiquiti UNVR (4-bay NAS/NVR)
- `alpine-v2-ubnt-udmpro.dts` — Ubiquiti UDM Pro (Gateway/Router)

### Configs (`configs/`)

- `unvr_defconfig` — Kernel config for UNVR (NAS focus: RAID, SATA, NFS, Samba)
- `udmpro_defconfig` — Kernel config for UDM Pro (Gateway focus: nftables, WireGuard, QoS, bridging)
- `Dockerfile` — Cross-compilation environment (Debian Bookworm, aarch64-linux-gnu)

## Key Discoveries

These findings are not documented anywhere else and were discovered through firmware reverse-engineering:

1. **AXI Sub-Master Snoop (SMCC)** — Internal PCI devices need SMCC registers (offset 0x110, 0x130, 0x150, 0x170) configured with bits 0+1 (SNOOP_OVR | SNOOP_ENABLE) for cache-coherent DMA. Without this, TX DMA reads return stale cache data.

2. **APP_CONTROL Register (0x220)** — Lower 16 bits must be set to 0x03FF for DMA to function. Discovered via `vmlinux-to-elf` disassembly of the stock firmware.

3. **DBI Base Offset** — On Alpine V2, DWC PCIe DBI registers are at `controller_base + 0x10000`, not at `controller_base`. The mainline `pcie-al.c` driver must pre-set `pci->dbi_base` to avoid a resource conflict with the DWC framework.

4. **ECAM Address** — External PCIe config space (ECAM) is at `0xfb600000` (1MB), not at `0xfd810000` (64KB). The 64KB region is the DBI, not ECAM.

5. **MSI-X Initialization** — The `al,alpine-msix` node requires `interrupt-controller` and `#interrupt-cells` properties for `of_irq_init()` to process it.

6. **`linux,pci-probe-only`** — Must be placed in the `/chosen` node (not the PCI controller node) and must be `= <1>` (u32 value, not boolean).

7. **IOMMU Passthrough** — `iommu.passthrough=1` must be in the DTB bootargs, not U-Boot env, because U-Boot's `setenv bootargs` in the boot command overrides DTB values.

8. **Shared MDIO Bus** — On Alpine V2, all Ethernet MACs share one physical MDIO bus. The stock firmware has a separate `alpine_mdio_shared` platform driver (out-of-tree). Our `al_eth` module implements shared MDIO internally: the first port with `phy_exist` registers the bus, subsequent ports reuse it.

9. **AT803X PHY requires REGULATOR** — The UDM Pro uses an Atheros/QCA 8031 PHY (driver `at803x`). This driver has `depends on REGULATOR` in Kconfig. Without `CONFIG_REGULATOR=y`, the driver is silently not built even when `CONFIG_AT803X_PHY=y` is set. This is a common Kconfig pitfall.

10. **EEPROM Identity (SPI-NOR Flash)** — Hardware identity (MAC, board ID, device ID, HW revision) is stored in the "eeprom" MTD partition (typically `/dev/mtd4ro`). The stock firmware reads this via `ubnthal.ko` at `/proc/ubnthal/board`. Without ubnthal, the EEPROM can be read directly. Layout:
    ```
    Offset  Size  Field
    0x0000  6     Base MAC address
    0x000C  2     Board ID (e.g. 0xEA15 = UDM Pro)
    0x000E  2     Hardware revision
    0x0010  4     Device ID (unique per unit)
    0x8000  4     Magic "UBNT" (redundant copy)
    ```

11. **Platform Detection without ubnthal** — On custom kernels without `ubnthal.ko`, the platform can be detected via DeviceTree (`/sys/firmware/devicetree/base/compatible` contains `ubnt,udm-pro`) and the board ID via `/proc/cmdline` (`boardid=ea15`) or EEPROM MTD.

12. **RTL8370MB Switch (UDM Pro)** — The 8-port GbE switch uses SMI-over-MDIO at **PHY address 0x1D (29)**, not at the DTS `reg = <0x11>` (which is an internal device ID). SMI management goes through **eth8's MDIO bus** (PCI 00:01.0), not eth0's. The switch requires PCA9575 GPIO pins 4+8 driven HIGH to release from hardware reset. See `docs/RTL8370MB.md` for full details.

13. **Switch Uplink phy_exist Override** — Board params for port 3 (switch uplink) say `phy_exist=Yes, phy_addr=17`, but the RTL8370MB doesn't respond to standard PHY reads. Setting `phy_exist=false` in the al_eth driver makes `al_eth_up` call `al_eth_mac_link_config` directly (fixed 1000M/FD). Without this, the RGMII MAC never gets speed/duplex configured and RX silently receives 0 bytes.

## Building

```bash
# Build kernel + all modules
docker build -t alpine-v2-builder -f configs/Dockerfile .

docker run --rm \
  -v /path/to/linux-6.12-stable:/src/linux:z \
  -v $(pwd)/configs/unvr_defconfig:/src/unvr_defconfig:z \
  -v $(pwd)/dts:/src/dts:z \
  -v $(pwd)/modules:/src/modules:z \
  -v $(pwd)/output:/output:z \
  alpine-v2-builder bash -c '
    cp -a /src/linux /build/linux
    cp /src/unvr_defconfig /build/linux/arch/arm64/configs/unvr_defconfig
    cp /src/dts/*.dts /build/linux/arch/arm64/boot/dts/amazon/
    cd /build/linux
    make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- unvr_defconfig
    make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j$(nproc) Image dtbs modules
    # Build out-of-tree modules (rtl8370mb depends on al_eth symbols)
    for mod in al_eth al_dma al_ssm al_sgpo; do
      cp -a /src/modules/$mod /build/$mod
      make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- W=0 M=/build/$mod modules
    done
    cp -a /src/modules/rtl8370mb /build/rtl8370mb
    make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- W=0 M=/build/rtl8370mb \
      KBUILD_EXTRA_SYMBOLS=/build/al_eth/Module.symvers modules
  '
```

For UDM Pro, replace `unvr_defconfig` with `udmpro_defconfig`.

## Hardware Support Status

| Feature | UNVR | UDM Pro | Driver |
|---------|------|---------|--------|
| CPU (4x Cortex-A57) | ✅ | ✅ | mainline |
| Internal PCIe | ✅ | ✅ | `pcie-al-internal` (new) |
| External PCIe | ✅ | ✅ | `pcie-al` (patched) |
| Ethernet 1GbE (RGMII) | ✅ | ✅ | `al_eth` + AT803X PHY |
| Ethernet 10GbE SFP+ | ✅ | ✅ | `al_eth` (LM mode) |
| Shared MDIO Bus | ✅ | ✅ | `al_eth` (built-in shared) |
| RTL8370MB 8-Port Switch | — | ✅ | `rtl8370mb` (new) |
| AHCI SATA | ✅ | ✅ | mainline `ahci` |
| RAID5/6 HW Parity | ✅ | ✅ | `al_dma` (ported) |
| HW AES Crypto | ✅ | ✅ | `al_ssm` (ported) |
| HDD Bay LEDs | ✅ | ✅ | `al_sgpo` (new) |
| Fan Control (ADT7475) | ✅ | ✅ | mainline `adt7475` |
| I2C GPIO Expanders | ✅ | ✅ | mainline `pca953x` |
| MSI-X Interrupts | ✅ | ✅ | mainline `irq-alpine-msi` |
| USB (xHCI) | ✅ | ✅ | mainline `xhci-hcd` |
| RTC (S35390A) | ✅ | ✅ | mainline |
| Watchdog (SP805) | ✅ | ✅ | mainline |
| SPI Flash (MTD/EEPROM) | ✅ | ✅ | mainline |
| LCM Display | — | ✅ | USB ACM (`ttyACM0`) |

## Tested On

- **Ubiquiti UNVR** (Board ID `ea1a`, 4GB RAM, 4x SATA bays)
  - Kernel: 6.12.77
  - Rootfs: Alpine Linux 3.21
  - RAID5: 3x 3TB HDD, 57 MB/s write, 204 MB/s read
  - HW Crypto: AES-XTS priority 400 (beats ARM CE at 300)
  - Fans: 3x, PWM controllable, 2600-8600 RPM

- **Ubiquiti UDM Pro** (Board ID `ea15`, 4GB RAM, 2x SFP+ / 2x 1GbE / RTL8370MB switch)
  - Kernel: 6.12.77
  - Rootfs: Alpine Linux 3.21
  - Switch: RTL8370MB fully operational (8x GbE LAN, 0.15ms RTT)
  - PHY: Atheros QCA8031 (AT803X driver, needs `CONFIG_REGULATOR=y`)
  - eMMC boot via USB xHCI (ASMedia)

## UDM Pro Network Ports

Stock firmware names (after interface renaming):

| Port | Interface | PCI Device | Speed | PHY/Switch | Description |
|------|-----------|------------|-------|------------|-------------|
| SFP+ WAN | eth9 | 00:00.0 | 10G | SFP LM | WAN SFP+ |
| WAN RJ45 | eth8 | 00:01.0 | 1G | QCA8031 (MDIO addr 4) | WAN RJ45 |
| SFP+ LAN | eth10 | 00:02.0 | 10G | SFP LM | LAN SFP+ |
| Switch Uplink | eth0 | 00:03.0 | 1G RGMII | RTL8370MB (SMI addr 0x1D) | 8-port LAN switch |
| LAN Ports 1-8 | (via eth0) | — | 1G | RTL8370MB internal PHY | RJ45 LAN |

## Platform-Specific Notes

### UDM Pro Kernel Config

The UDM Pro defconfig requires these non-obvious settings:

```
CONFIG_REGULATOR=y                # Required dependency for AT803X PHY driver
CONFIG_REGULATOR_FIXED_VOLTAGE=y  # Needed by regulator framework
CONFIG_AT803X_PHY=y               # Atheros/QCA 8031 PHY (eth8 WAN RJ45)
CONFIG_REALTEK_PHY=y              # RTL8370MB switch PHY
```

Without `CONFIG_REGULATOR`, `CONFIG_AT803X_PHY=y` is silently ignored by Kconfig (`depends on REGULATOR`).

### Docker Export Caveat

When building rootfs via `docker export`, the exported tar contains `/dev/console` as a regular file (not a character device). This prevents `devtmpfs` from mounting at boot, causing a kernel panic ("No working init found"). Fix: `rm -rf $ROOTFS/dev && mkdir -p $ROOTFS/dev` after extraction.

## License

- Kernel patches: GPL-2.0
- Out-of-tree modules: GPL-2.0 (based on Annapurna Labs HAL, dual-licensed GPL/commercial)
- Device trees: GPL-2.0 OR MIT

## Credits

- [delroth](https://github.com/delroth) — al_eth standalone driver, Alpine HAL extraction
- [riptidewave93](https://github.com/riptidewave93) — UNVR-NAS project, boot chain documentation
- [Bootlin](https://bootlin.com) — Original Alpine V2 upstreaming effort (2017)
- Annapurna Labs / Amazon — Original HAL source code
