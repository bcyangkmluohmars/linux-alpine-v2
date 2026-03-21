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
| `al_eth` | 1GbE + 10GbE Ethernet driver | Ported from [delroth/al_eth-standalone](https://github.com/delroth/al_eth-standalone) (5.5 → 6.12) |
| `al_dma` | RAID5/6 hardware parity acceleration (XOR/PQ) | HAL from [delroth/alpine_hal](https://github.com/delroth/alpine_hal) |
| `al_ssm` | Hardware AES-XTS/CBC crypto engine | HAL from alpine_hal |
| `al_sgpo` | Serial GPIO Output controller for HDD bay LEDs | Reverse-engineered from firmware |

### Device Trees (`dts/`)

- `alpine-v2-ubnt-unvr.dts` — Ubiquiti UNVR (4-bay NAS/NVR)

### Configs (`configs/`)

- `unvr_defconfig` — Minimal kernel config for UNVR
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
    # Build out-of-tree modules
    for mod in al_eth al_dma al_ssm al_sgpo; do
      cp -a /src/modules/$mod /build/$mod
      make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- W=0 M=/build/$mod modules
    done
  '
```

## Hardware Support Status

| Feature | Status | Driver |
|---------|--------|--------|
| CPU (4x Cortex-A57) | ✅ | mainline |
| Internal PCIe | ✅ | `pcie-al-internal` (new) |
| External PCIe | ✅ | `pcie-al` (patched) |
| Ethernet 1GbE | ✅ | `al_eth` (ported) |
| Ethernet 10GbE SFP+ | ✅ | `al_eth` (ported) |
| AHCI SATA | ✅ | mainline `ahci` |
| RAID5/6 HW Parity | ✅ | `al_dma` (ported) |
| HW AES Crypto | ✅ | `al_ssm` (ported) |
| HDD Bay LEDs | ✅ | `al_sgpo` (new) |
| Fan Control (ADT7475) | ✅ | mainline `adt7475` |
| I2C GPIO Expanders | ✅ | mainline `pca953x` |
| MSI-X Interrupts | ✅ | mainline `irq-alpine-msi` |
| USB (xHCI) | ✅ | mainline `xhci-hcd` |
| RTC (S35390A) | ✅ | mainline |
| Watchdog (SP805) | ✅ | mainline |
| SPI Flash (MTD) | ✅ | mainline |

## Tested On

- **Ubiquiti UNVR** (Board ID `ea1a`, 4GB RAM, 4x SATA bays)
  - Kernel: 6.12.77
  - Rootfs: Alpine Linux 3.21
  - RAID5: 3x 3TB HDD, 57 MB/s write, 204 MB/s read
  - HW Crypto: AES-XTS priority 400 (beats ARM CE at 300)
  - Fans: 3x, PWM controllable, 2600-8600 RPM

## License

- Kernel patches: GPL-2.0
- Out-of-tree modules: GPL-2.0 (based on Annapurna Labs HAL, dual-licensed GPL/commercial)
- Device trees: GPL-2.0 OR MIT

## Credits

- [delroth](https://github.com/delroth) — al_eth standalone driver, Alpine HAL extraction
- [riptidewave93](https://github.com/riptidewave93) — UNVR-NAS project, boot chain documentation
- [Bootlin](https://bootlin.com) — Original Alpine V2 upstreaming effort (2017)
- Annapurna Labs / Amazon — Original HAL source code
