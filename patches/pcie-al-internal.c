// SPDX-License-Identifier: GPL-2.0
/*
 * PCIe host controller driver for the Annapurna Labs Alpine V2 internal bus.
 *
 * The Alpine V2 SoC has an internal PCIe-like bus (bus 0 only) that hosts
 * integrated peripherals: two AHCI SATA controllers, Ethernet adapters,
 * a crypto engine, and a RAID/DMA engine. These are memory-mapped SoC
 * units that present a standard PCI configuration space interface via an
 * ECAM-compatible window at 0xfbc00000.
 *
 * Unlike real PCIe devices, these internal units require explicit AXI
 * Sub-Master Configuration & Control (SMCC) register setup to enable
 * cache-coherent DMA snooping. Without this, DMA transactions (especially
 * TX/M2S reads from main memory) will observe stale data because the
 * CPU cache is not snooped.
 *
 * This driver replaces the generic pci-host-ecam-generic driver with one
 * that additionally:
 *   1. Enables snoop (SNOOP_OVR | SNOOP_ENABLE) on SMCC sub-master 0
 *      for every device on the internal bus.
 *   2. For devices at slot <= 5 (Ethernet, SATA): also configures
 *      sub-masters 1, 2, and 3.
 *   3. Configures the APP_CONTROL register on ALL devices.
 *   4. Preserves firmware-assigned BAR values (PCI_PROBE_ONLY via DT).
 *
 * The SMCC and APP_CONTROL configuration is performed via a PCI bus
 * notifier that fires when drivers bind to devices, exactly matching
 * the stock kernel's approach.
 *
 * Reverse-engineered from the stock Ubiquiti UNVR firmware kernel
 * (4.19, Annapurna Labs out-of-tree driver: pci-internal-alpine.c,
 *  function: al_pci_internal_device_notifier).
 *
 * Copyright (c) 2025 secfirstnas-rs project
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_pci.h>
#include <linux/pci.h>
#include <linux/pci-ecam.h>
#include <linux/platform_device.h>

/*
 * AXI Sub-Master Configuration & Control (SMCC) registers.
 * Located at PCI config space offsets within each device's 4KB window.
 *
 * Each device has up to 4 AXI sub-masters. Each sub-master's SMCC
 * register block is AL_ADAPTER_SMCC_BUNDLE_SIZE bytes apart.
 *
 * Sub-master 0: offset 0x110
 * Sub-master 1: offset 0x130
 * Sub-master 2: offset 0x150
 * Sub-master 3: offset 0x170
 */
#define AL_ADAPTER_SMCC			0x110
#define AL_ADAPTER_SMCC_BUNDLE_SIZE	0x20
#define AL_ADAPTER_SMCC_CONF_SNOOP_OVR	BIT(0)
#define AL_ADAPTER_SMCC_CONF_SNOOP_EN	BIT(1)
#define AL_ADAPTER_SMCC_SNOOP_BITS	(AL_ADAPTER_SMCC_CONF_SNOOP_OVR | \
					 AL_ADAPTER_SMCC_CONF_SNOOP_EN)

/* Number of AXI sub-masters per device */
#define AL_ADAPTER_SMCC_NUM_SUBMASTERS	4

/*
 * Application Control register.
 * The stock driver sets the lower 10 bits to 0x3ff on ALL devices,
 * and clears bits [15:10] (via MOVK instruction replacing the lower
 * 16 bits with 0x03ff, preserving upper 16 bits).
 */
#define AL_ADAPTER_APP_CONTROL		0x220
#define AL_ADAPTER_APP_CONTROL_LO16	0x03ff

/*
 * Device slot threshold for sub-master configuration:
 * - Slot <= 5: SMCC snoop is configured on ALL 4 sub-masters
 * - Slot >  5: SMCC snoop is configured on sub-master 0 only
 * ALL devices get APP_CONTROL configured regardless of slot.
 */
#define AL_INTERNAL_SLOT_THRESHOLD	5

/* Annapurna Labs PCI vendor ID */
#define PCI_VENDOR_ID_ANNAPURNA_LABS	0x1c36

/*
 * Bus notifier callback: configures SMCC snoop and APP_CONTROL
 * registers when a driver is about to bind to a device on the
 * internal PCIe bus.
 *
 * This mirrors the stock kernel's al_pci_internal_device_notifier().
 */
static int al_pcie_internal_notifier(struct notifier_block *nb,
				     unsigned long event, void *data)
{
	struct device *dev = data;
	struct pci_dev *pdev;
	unsigned int slot;
	u32 val;
	int i;

	/* Only act when a driver is about to bind */
	if (event != BUS_NOTIFY_BIND_DRIVER)
		return NOTIFY_DONE;

	/* Only process PCI devices */
	if (!dev_is_pci(dev))
		return NOTIFY_DONE;

	pdev = to_pci_dev(dev);

	/*
	 * Only process function 0 of each device.
	 * The stock driver checks (devfn & 0x7) == 0.
	 */
	if (PCI_FUNC(pdev->devfn) != 0)
		return NOTIFY_DONE;

	/*
	 * Only process devices on the root bus (bus 0) of the internal
	 * PCIe controller. Skip devices on subordinate buses (e.g.,
	 * external PCIe devices behind a bridge). The stock driver
	 * checks that pdev->bus->self is NULL (root bus indicator).
	 *
	 * Also filter by vendor ID to avoid writing Alpine-specific
	 * SMCC registers into non-AL devices' config space.
	 */
	if (!pci_is_root_bus(pdev->bus))
		return NOTIFY_DONE;

	if (pdev->vendor != PCI_VENDOR_ID_ANNAPURNA_LABS)
		return NOTIFY_DONE;

	slot = PCI_SLOT(pdev->devfn);

	/*
	 * Step 1: Configure SMCC snoop bits on sub-master 0.
	 * Read-modify-write: set SNOOP_OVR (bit 0) and SNOOP_EN (bit 1).
	 */
	pci_read_config_dword(pdev, AL_ADAPTER_SMCC, &val);
	val |= AL_ADAPTER_SMCC_SNOOP_BITS;
	pci_write_config_dword(pdev, AL_ADAPTER_SMCC, val);

	/*
	 * Step 2: For devices at slot <= 5 (Ethernet adapters, SATA
	 * controllers), also configure sub-masters 1, 2, and 3.
	 * The stock driver writes the same modified value from sub-master 0.
	 */
	if (slot <= AL_INTERNAL_SLOT_THRESHOLD) {
		for (i = 1; i < AL_ADAPTER_SMCC_NUM_SUBMASTERS; i++) {
			u32 offset = AL_ADAPTER_SMCC +
				     (i * AL_ADAPTER_SMCC_BUNDLE_SIZE);
			pci_write_config_dword(pdev, offset, val);
		}
	}

	/*
	 * Step 3: Configure APP_CONTROL register on ALL devices.
	 * The stock driver uses MOVK to replace the lower 16 bits
	 * with 0x03ff, preserving the upper 16 bits.
	 */
	pci_read_config_dword(pdev, AL_ADAPTER_APP_CONTROL, &val);
	val = (val & 0xffff0000) | AL_ADAPTER_APP_CONTROL_LO16;
	pci_write_config_dword(pdev, AL_ADAPTER_APP_CONTROL, val);

	dev_info(dev, "al-internal-pcie: SMCC snoop configured (slot %u, %s sub-masters)\n",
		 slot,
		 slot <= AL_INTERNAL_SLOT_THRESHOLD ? "all 4" : "SM0 only");

	return NOTIFY_OK;
}

static struct notifier_block al_pcie_internal_nb = {
	.notifier_call = al_pcie_internal_notifier,
};

/*
 * ECAM init callback: registers the bus notifier for SMCC snoop
 * configuration. Called by pci_host_common_probe() after the config
 * window is mapped but before bus scanning starts.
 */
static int al_pcie_internal_ecam_init(struct pci_config_window *cfg)
{
	int ret;

	ret = bus_register_notifier(&pci_bus_type, &al_pcie_internal_nb);
	if (ret) {
		pr_err("al-internal-pcie: failed to register bus notifier (%d)\n",
		       ret);
		return ret;
	}

	pr_info("al-internal-pcie: registered SMCC snoop notifier\n");
	return 0;
}

static const struct pci_ecam_ops al_pcie_internal_ecam_ops = {
	.pci_ops	= {
		.map_bus	= pci_ecam_map_bus,
		.read		= pci_generic_config_read,
		.write		= pci_generic_config_write,
	},
	.init		= al_pcie_internal_ecam_init,
};

static const struct of_device_id al_pcie_internal_of_match[] = {
	{
		.compatible = "annapurna-labs,alpine-internal-pcie",
		.data = &al_pcie_internal_ecam_ops,
	},
	{ },
};
MODULE_DEVICE_TABLE(of, al_pcie_internal_of_match);

static struct platform_driver al_pcie_internal_driver = {
	.driver = {
		.name = "al-internal-pcie",
		.of_match_table = al_pcie_internal_of_match,
		.suppress_bind_attrs = true,
	},
	.probe = pci_host_common_probe,
	.remove_new = pci_host_common_remove,
};
builtin_platform_driver(al_pcie_internal_driver);

MODULE_DESCRIPTION("Annapurna Labs Alpine V2 internal PCIe host controller");
MODULE_LICENSE("GPL v2");
