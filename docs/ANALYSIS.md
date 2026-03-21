# Analysis: TX DMA Failure on Alpine V2 (UNVR) with Kernel 6.12

## Executive Summary

**Root cause identified**: The stock `annapurna-labs,alpine-internal-pcie` driver performs
critical SoC-level initialization that the generic `pci-host-ecam-generic` driver does not:

1. **AXI Sub-Master snoop configuration** (`al_pcie_port_snoop_config` / `al_unit_adapter_snoop_enable`)
2. **Port configuration** (`al_pcie_port_config`) including ATU, AXI QoS, and coherency settings
3. **PF handle initialization** (`al_pcie_pf_handle_init` / `al_pcie_port_handle_init`)

These configure the SoC fabric to allow DMA devices on the internal PCIe bus to properly
issue AXI write transactions (TX = M2S = Memory-to-Stream). Without them, the UDMA TX
engine enters NORMAL state but its AXI write transactions are silently dropped or
misdirected by the system fabric.

**Why RX works but TX doesn't**: On the Alpine V2, the RX path (S2M = Stream-to-Memory)
writes DMA data to main memory. The TX path (M2S) reads descriptor data and packet data
from main memory. The asymmetry comes from how the AXI fabric handles read vs write
transactions from the internal bus masters. The U-Boot bootloader partially initializes
the internal PCIe for SATA (which also works), but does NOT fully initialize the Ethernet
adapter's AXI sub-master snoop/coherency configuration for M2S operations.

## Detailed Analysis

### 1. Stock `alpine-internal-pcie` Driver Functions (from binary symbols)

The stock kernel binary contains these critical functions that have NO equivalent in
`pci-host-ecam-generic`:

```
al_pcie_port_handle_init      - Initialize PCIe port handle (maps registers)
al_pcie_port_config            - Full port configuration
al_pcie_port_config_live       - Live port reconfiguration
al_pcie_port_snoop_config      - Configure snoop attributes on internal bus
al_pcie_port_enable            - Enable the port
al_pcie_pf_handle_init         - Initialize Physical Function handle
al_pcie_pf_config              - Configure PF settings
al_pcie_axi_qos_config         - AXI Quality of Service configuration
al_pcie_axi_io_config          - AXI I/O configuration
al_pcie_port_ib_hcrd_os_ob_reads_config - Inbound header credits / OB reads
al_pcie_atu_region_set         - Address Translation Unit region setup
al_unit_adapter_snoop_enable   - Enable snooping on unit adapter
```

The generic ECAM driver does NONE of this. It only maps the ECAM config space window
and provides standard PCI config read/write operations.

### 2. The Snoop Configuration Problem

The Alpine V2 SoC uses AXI Sub-Master Configuration & Control (SMCC) registers at
offset 0x110 within each PCI device's config space. These registers control:

```c
#define AL_ADAPTER_SMCC                 0x110
#define AL_ADAPTER_SMCC_CONF_SNOOP_OVR  AL_BIT(0)  // Snoop Override
#define AL_ADAPTER_SMCC_CONF_SNOOP_ENABLE AL_BIT(1) // Snoop Enable
```

The `al_pcie_port_snoop_config` function configures the PCIe port-level snoop policy.
The `al_unit_adapter_snoop_enable` function enables snooping for each individual
adapter (Ethernet, SATA, Crypto, DMA) on the internal bus.

**Snooping is critical for DMA coherency on ARM64**. When an internal bus master
(like the Ethernet UDMA engine) reads from or writes to main memory, the AXI
transaction must carry the correct snoop attributes so the CCU (Cache Coherency
Unit) can handle cache coherence. Without proper snoop configuration:

- **Reads** (descriptor fetch, data fetch for TX) may return stale cached data
- **Writes** (RX data writes) may bypass the cache but still reach memory

This explains the asymmetry: RX writes go directly to memory (which works even
without snooping if the kernel handles cache maintenance), but TX reads may
fetch stale/incorrect descriptor and buffer data.

### 3. Device Tree Comparison

**Stock DTS** (4.19 kernel):
```
pcie-internal {
    dma-coherent;
    compatible = "annapurna-labs,alpine-internal-pcie";
    device_type = "pci";
    reg = <0x00 0xfbc00000 0x00 0x100000>;
    reg-names = "ecam";
    bus-range = <0x00 0x00>;
    ...
};
```

**Our DTS** (6.12 kernel):
```
pci@fbc00000 {
    compatible = "pci-host-ecam-generic";
    dma-coherent;
};
```

The `dma-coherent` property tells the kernel to skip software cache maintenance
for DMA buffers. This is CORRECT when the hardware handles coherence (via snoop
config). But we have `dma-coherent` set WITHOUT the hardware snoop actually being
configured -- the generic ECAM driver doesn't know how to do this.

**Also notable**: The stock DTS has a CCU node with `io_coherency = <1>`:
```
ccu {
    compatible = "annapurna-labs,al-ccu";
    reg = <0x00 0xf0090000 0x00 0x10000>;
    io_coherency = <0x01>;
};
```

This Cache Coherency Unit driver is part of the out-of-tree Alpine platform code
that sets up IO coherency at the SoC level. Our kernel does NOT have this driver.

### 4. The `IS_NIC` vs `ALPINE_INTEGRATED` Code Path

The al_eth driver has two modes:

- **`ALPINE_INTEGRATED` (board_type=0)**: Device on internal SoC bus. BARs mapped
  directly. Uses PCI device ID 0x0001 (AL_ETH). The driver calls `pci_set_master(pdev)`
  which goes through the standard PCI config space write.

- **`IS_NIC` (board_types 1-4)**: Device on external PCIe NIC card. The driver
  explicitly configures:
  - Target-ID (tgtid) for UDMA queues to route DMA through the correct PCIe port
  - MSIX tgtid configuration
  - PCI_COMMAND_MASTER and PCI_COMMAND_MEMORY on the adapter's config space

For `ALPINE_INTEGRATED`, the driver **skips** tgtid configuration (line 868:
`if (IS_NIC(adapter->board_type))`). This is correct because on the internal bus,
the devices don't need tgtid routing -- BUT this means the **snoop configuration
must come from elsewhere**, namely the `alpine-internal-pcie` platform driver.

### 5. What `pci_set_master` Does and Why It's Insufficient

`pci_set_master()` sets the PCI_COMMAND_MASTER bit (bit 2) in the PCI Command
register. On a real PCIe bus, this enables the device to issue bus master
transactions. On the Alpine V2 internal bus, these are not real PCIe devices --
they are memory-mapped units on the SoC fabric that present a PCI config space
interface.

Setting PCI_COMMAND_MASTER in the standard PCI config space is necessary but
not sufficient. The **AXI sub-master configuration** (SMCC registers at
offset 0x110) must also be configured with proper snoop attributes. This is
what the stock `alpine-internal-pcie` driver does that `pci-host-ecam-generic`
does not.

### 6. UNVR-NAS (RIPTIDEWAVE93) Project Analysis

The RIPTIDEWAVE93/UNVR-NAS project:
- Uses the **stock Ubiquiti kernel** from the GPL source (udm-kernel from
  fabianishere, which is the Ubiquiti 4.19 kernel with Alpine support)
- Builds with `alpine_v2_defconfig` which includes:
  - `CONFIG_PCI_INTERNAL_ALPINE=y`
  - `CONFIG_PCI_EXTERNAL_ALPINE=y`
  - `CONFIG_PCI_EXTERNAL_ERR_ALPINE=y`
  - `# CONFIG_IOMMU_SUPPORT is not set`
- **Does NOT use a mainline kernel** -- they use the stock Ubiquiti kernel
  with its full out-of-tree Alpine platform code
- **No PCIe patches** -- the stock kernel's internal PCIe driver handles
  everything

This means they don't face this issue because they have the full
`annapurna-labs,alpine-internal-pcie` driver.

### 7. Kernel Config Comparison

**Stock 4.19 config** (relevant entries):
```
CONFIG_ARCH_ALPINE=y
CONFIG_PCI_INTERNAL_ALPINE=y     <-- Internal PCIe driver (NOT in mainline)
CONFIG_PCI_EXTERNAL_ALPINE=y
CONFIG_PCI_EXTERNAL_ERR_ALPINE=y
# CONFIG_PCI_HOST_GENERIC is not set  <-- Not using ECAM generic!
CONFIG_NET_AL_ETH=y
CONFIG_ALPINE_SERDES_AVG=y
CONFIG_ALPINE_SERDES_HSSP=y
# CONFIG_IOMMU_SUPPORT is not set
ALPINE_PLATFORM="ALPINE_V2"
CONFIG_ALPINE_IOFIC=y
```

**Our 6.12 defconfig**:
```
CONFIG_ARCH_ALPINE=y
CONFIG_PCI=y
CONFIG_PCIE_AL=y               <-- External PCIe only (DWC-based)
CONFIG_PCI_HOST_GENERIC=y      <-- Using ECAM generic for internal bus
# No CONFIG_PCI_INTERNAL_ALPINE (doesn't exist in mainline)
# No CONFIG_ALPINE_IOFIC
```

The key gap: `CONFIG_PCI_INTERNAL_ALPINE` and `CONFIG_ALPINE_IOFIC` do not exist
in mainline Linux. They are part of the out-of-tree Annapurna Labs HAL codebase.

## Solution Options

### Option A: Write a Minimal `alpine-internal-pcie` Quirk Driver (Recommended)

Create a PCI fixup or a minimal platform driver that runs during PCI enumeration
and configures the snoop/SMCC registers for each device on the internal bus.

**What it needs to do (at minimum)**:

1. For each PCI device on bus 0 of the internal PCIe (at 0xfbc00000):
   - Write to the SMCC registers at PCI config offset 0x110 in each device:
     ```c
     /* Enable snoop override and snoop enable for all sub-masters */
     for (sm = 0; sm < 4; sm++) {
         u32 smcc_addr = AL_ADAPTER_SMCC + (sm * AL_ADAPTER_SMCC_BUNDLE_SIZE);
         val = readl(ecam_base + devfn_offset + smcc_addr);
         val |= AL_ADAPTER_SMCC_CONF_SNOOP_OVR | AL_ADAPTER_SMCC_CONF_SNOOP_ENABLE;
         writel(val, ecam_base + devfn_offset + smcc_addr);
     }
     ```

2. Ensure PCI_COMMAND has MASTER and MEMORY bits set (pci_set_master already
   does this, but verify the write takes effect on the internal bus).

**Implementation approach**: Use a PCI `DECLARE_PCI_FIXUP_FINAL` for vendor ID
0x1c36 (Annapurna Labs) that configures the SMCC snoop registers.

### Option B: Direct Register Poke from the al_eth Driver

Modify the `al_eth_hw_init_adapter` function to also configure snoop registers
when `board_type == ALPINE_INTEGRATED`, even though the stock driver doesn't
(because the stock kernel's internal PCIe driver handles it).

Add after line 864 (`rc = al_eth_adapter_init(&adapter->hal_adapter, params)`):

```c
if (adapter->board_type == ALPINE_INTEGRATED) {
    /* Configure AXI sub-master snoop for cache coherent DMA.
     * On stock kernel this is done by the alpine-internal-pcie driver.
     * With pci-host-ecam-generic, we must do it ourselves.
     */
    void __iomem *cfg_base = adapter->udma_base;  /* or compute from adapter base */
    int sm;
    for (sm = 0; sm < 4; sm++) {
        u32 smcc_off = 0x110 + (sm * 0x20);
        u32 val;
        pci_read_config_dword(adapter->pdev, smcc_off, &val);
        val |= 0x3;  /* SNOOP_OVR | SNOOP_ENABLE */
        pci_write_config_dword(adapter->pdev, smcc_off, val);
    }
}
```

### Option C: Write a Standalone Kernel Module

Create a small kernel module that runs early and configures all internal
PCIe devices' SMCC registers by directly mmapping the ECAM space at
0xfbc00000 and writing to the appropriate offsets.

### Option D: U-Boot Fix

Configure the snoop registers in U-Boot before booting the kernel.
This is the cleanest approach if you have U-Boot source access, but
depends on the U-Boot version supporting these register writes.

## Verification Steps

To confirm this analysis, on the running UNVR with kernel 6.12:

1. **Read the SMCC register for the Ethernet adapter**:
   ```bash
   # Assuming devmem2 or similar is available
   # Internal PCIe ECAM at 0xfbc00000, Ethernet at some device function
   # SMCC at offset 0x110 from the device's config base
   devmem2 <ecam_base + dev_offset + 0x110> w
   ```
   If the value does NOT have bits 0-1 set, snoop is not configured.

2. **Read the PCI Command register**:
   ```bash
   lspci -s 00:xx.0 -vv | grep "Bus Master"
   ```
   Verify bus mastering is enabled.

3. **Check UDMA status registers**:
   The M2S (TX) UDMA state register should show 0x2222 (all sub-engines in
   NORMAL state). If the descriptor prefetcher sub-engine is IDLE while others
   are NORMAL, it suggests the prefetcher can't read descriptors from memory
   (snoop/coherency issue).

4. **Test by writing snoop registers manually**:
   ```bash
   # If you have devmem2, try enabling snoop on the Ethernet device
   # and then try to transmit
   devmem2 <addr_of_smcc> w 0x3
   ```

## Key Registers and Addresses

| Register | Offset | Description |
|----------|--------|-------------|
| ECAM base | 0xfbc00000 | Internal PCIe configuration space |
| SMCC | 0x110 | AXI Sub-Master Config (per device) |
| SMCC bundle | 0x20 | Spacing between sub-master configs |
| SMCC_SNOOP_OVR | bit 0 | Override default snoop setting |
| SMCC_SNOOP_ENABLE | bit 1 | Enable cache-coherent snooping |
| CCU base | 0xf0090000 | Cache Coherency Unit |
| NB Service | 0xf0070000 | System fabric (Northbridge service) |
| PCI Command | 0x04 | Standard PCI command register |

## Files Referenced

- Stock kernel binary: `/run/media/kevin/KioxiaNVMe/sec/secfirstnas-rs/kernel/reference/firmware/_3488-UNVR-5.0.13-5fc20899-54b4-44a2-a958-f3b210adf9da.bin.extracted/154DE0`
- Stock config: `/run/media/kevin/KioxiaNVMe/sec/secfirstnas-rs/kernel/reference/ubiquiti-4.19-config`
- Stock DTS: `/run/media/kevin/KioxiaNVMe/sec/secfirstnas-rs/kernel/reference/ubiquiti-unvr-live.dts`
- Our DTS: `/run/media/kevin/KioxiaNVMe/sec/secfirstnas-rs/kernel/dts/alpine-v2-ubnt-unvr.dts`
- Our al_eth driver: `/run/media/kevin/KioxiaNVMe/sec/secfirstnas-rs/kernel/modules/al_eth/`
- SMCC register defs: `/run/media/kevin/KioxiaNVMe/sec/secfirstnas-rs/kernel/modules/al_eth/al_hal_unit_adapter_regs.h`
- Mainline ECAM driver: `/run/media/kevin/KioxiaNVMe/sec/secfirstnas-rs/kernel/reference/linux-6.12-stable/drivers/pci/controller/pci-host-generic.c`
- Mainline pcie-al.c: `/run/media/kevin/KioxiaNVMe/sec/secfirstnas-rs/kernel/reference/linux-6.12-stable/drivers/pci/controller/dwc/pcie-al.c`
- Mainline alpine-v2.dtsi: `/run/media/kevin/KioxiaNVMe/sec/secfirstnas-rs/kernel/reference/linux-6.12-stable/arch/arm64/boot/dts/amazon/alpine-v2.dtsi`

## Additional Considerations

### Why AHCI SATA Works

AHCI SATA works because:
1. U-Boot initializes the SATA controller for boot (eMMC on UNVR is via USB/xHCI,
   but U-Boot may still initialize SATA for its own use)
2. The AHCI driver in mainline Linux has its own DMA setup that may be more
   resilient to missing snoop configuration
3. SATA may use a different AXI master path that is pre-configured by U-Boot

### The `dma-coherent` Property Interaction

With `dma-coherent` in the DTS and no actual hardware coherency configured:
- The kernel skips `dma_sync_*` calls (cache maintenance)
- If the hardware snoop is not enabled, DMA buffers may contain stale cache data
- This is a **silent data corruption** risk, not just a performance issue
- For TX: the UDMA reads descriptor/buffer data that may be stale in cache
- For RX: the CPU reads received data that may be cached copies of old data

If we fix the snoop configuration, `dma-coherent` becomes correct and everything
should work. If we CANNOT fix snoop configuration, we must REMOVE `dma-coherent`
and let the kernel do software cache maintenance.

### The `io_coherency` CCU Driver

The stock kernel has a CCU driver (`annapurna-labs,al-ccu`) with `io_coherency = <1>`.
This likely configures the CCI-500 (or similar) cache coherence interconnect to
enable IO coherency for AXI masters. Our kernel does not have this driver.

However, U-Boot likely configures CCU IO coherency during boot (since SATA works
with DMA). The missing piece is the **per-device snoop attribute** configuration
in the SMCC registers, which tells the CCU to actually snoop for each specific
device's transactions.

Think of it as two levels:
1. **CCU level**: "IO coherency is available" (likely already configured by U-Boot)
2. **Device level**: "This device's AXI transactions should be snooped" (NOT configured)

The stock `alpine-internal-pcie` driver handles level 2. We need to replicate this.

## Implemented Solution: `pcie-al-internal.c` Driver

**Option A was implemented** as a minimal platform driver for kernel 6.12.

### Disassembly Findings

The stock kernel binary was converted to ELF using `vmlinux-to-elf` and key functions
were disassembled with `aarch64-linux-gnu-objdump`. The critical function
`al_pci_internal_device_notifier` at `0xffffff80084844c0` revealed:

1. **Event filter**: Only processes `BUS_NOTIFY_BIND_DRIVER` (event == 4)
2. **Device filter**: Only function 0 (`devfn & 0x7 == 0`), root bus only
3. **SMCC sub-master 0** (`0x110`): Read, OR with `0x3` (SNOOP_OVR|SNOOP_EN), write back
4. **Slot check**: `ubfx x0, x0, #3, #5` extracts slot number, `cmp w0, #5`
   - Slot <= 5: ALSO writes the snoop-modified value to SM1 (`0x130`), SM2 (`0x150`), SM3 (`0x170`)
   - Then falls through unconditionally to:
5. **APP_CONTROL** (`0x220`): Read, `movk w2, #0x3ff` replaces lower 16 bits with `0x03ff`, write back

Key insight: ALL devices get APP_CONTROL configured. The slot <= 5 branch configures
extra sub-masters AND then falls through to the APP_CONTROL write.

### Driver Implementation

File: `drivers/pci/controller/pcie-al-internal.c`

- Compatible string: `"annapurna-labs,alpine-internal-pcie"` (matches stock DTS)
- Based on `pci_host_common_probe` / `pci_ecam_ops` infrastructure
- Registers a `BUS_NOTIFY_BIND_DRIVER` notifier that configures SMCC and APP_CONTROL
- Filters by: function 0 only, root bus only, vendor ID `0x1c36` (Annapurna Labs)
- Config symbol: `CONFIG_PCIE_AL_INTERNAL=y`

### Files Modified

- **New**: `kernel/reference/linux-6.12-stable/drivers/pci/controller/pcie-al-internal.c`
- **Modified**: `kernel/reference/linux-6.12-stable/drivers/pci/controller/Kconfig` (added PCIE_AL_INTERNAL)
- **Modified**: `kernel/reference/linux-6.12-stable/drivers/pci/controller/Makefile` (added build rule)
- **Modified**: `kernel/dts/alpine-v2-ubnt-unvr.dts` (compatible changed from `pci-host-ecam-generic` to `annapurna-labs,alpine-internal-pcie`)
- **Modified**: `kernel/build/unvr_defconfig` (added `CONFIG_PCIE_AL_INTERNAL=y`)
