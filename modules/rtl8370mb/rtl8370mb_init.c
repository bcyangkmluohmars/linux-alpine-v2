// SPDX-License-Identifier: GPL-2.0
/*
 * rtl8370mb_init.c — RTL8370MB switch init + userspace SMI for UDM Pro
 *
 * SMI-over-MDIO at PHY address 0x1D (Realtek default) on eth1's MDIO bus.
 * See kernel/RTL8370MB.md for full hardware documentation.
 *
 * Initialization:
 *   1. Detect chip via SMI (chip_id=0x6368)
 *   2. Configure EXT1 as RGMII, force 1000M/FD/link-up
 *   3. Set port isolation: all LAN ports + CPU port
 *   4. Set STP state to forwarding
 *   5. Enable MAC learning
 *
 * Userspace SMI:
 *   /dev/rtl8370mb — misc chardev for atomic SMI read/write.
 *   The MDIO bus mutex is held for the full 4-op SMI sequence,
 *   so PHY polling cannot corrupt the transaction.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/phy.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>

/* ── SMI-over-MDIO protocol ──────────────────────────────────────────── */
#define SMI_CTRL0_REG		31
#define SMI_CTRL1_REG		21
#define SMI_ADDRESS_REG		23
#define SMI_DATA_WRITE_REG	24
#define SMI_DATA_READ_REG	25
#define SMI_ADDR_OP		0x000E
#define SMI_READ_OP		0x0001
#define SMI_WRITE_OP		0x0003

/* ── RTL8370MB registers (from mainline rtl8365mb.c + stock RE) ────── */
#define CHIP_ID_REG		0x1300
#define CHIP_VER_REG		0x1301
#define MAGIC_REG		0x13C2
#define MAGIC_VALUE		0x0249

/* EXT interface mode select (EXT0 bits [3:0], EXT1 bits [7:4]) */
#define DI_SELECT_REG0		0x1305
#define EXT_MODE_DISABLE	0
#define EXT_MODE_RGMII		1
#define EXT_MODE_SGMII		9
#define EXT_MODE_HSGMII		10	/* 2.5G SGMII */

/* EXT1 RGMII TX/RX delay */
#define EXT_RGMXF_REG1		0x1307
#define RGMXF_TXDELAY		0x0008
#define RGMXF_RXDELAY_1	0x0001

/* EXT1 force config */
#define DI_FORCE_REG1		0x1311
#define FORCE_EN		0x1000
#define FORCE_LINK		0x0010
#define FORCE_DUPLEX		0x0004
#define FORCE_SPEED_1000M	0x0002

/* CPU port */
#define CPU_PORT_MASK_REG	0x1219
#define CPU_CTRL_REG		0x121A
#define CPU_CTRL_EN		0x0001
#define CPU_CTRL_INSERTMODE_NONE 0x0004
#define CPU_CTRL_TRAP_PORT_1	0x0008

/* Port isolation */
#define PORT_ISO_BASE		0x08A2

/* STP state */
#define MSTI_CTRL_BASE		0x0A00
#define STP_FORWARDING		3

/* MAC learning limit */
#define LUT_LEARN_LIMIT_BASE	0x0A20
#define LEARN_LIMIT_MAX		2112

/* Max frame length */
#define CFG0_MAX_LEN_REG	0x088C
#define CFG0_MAX_LEN_MAX	0x3FFF

/* ── UDM Pro specifics ───────────────────────────────────────────────── */
#define SMI_PHY_ADDR		0x1D	/* Realtek default, verified by disasm */
#define NUM_PHY_PORTS		8
#define EXT1_PORT		9
#define ALL_PORTS_MASK		0x02FF	/* ports 0-7 + port 9 */
#define NUM_PORTS		11

#define SMI_RETRIES		10
#define SMI_RETRY_DELAY_MS	10

/* Shared MDIO bus from al_eth (on eth1) */
extern struct mii_bus *alpine_shared_mdio_bus;

static struct mii_bus *rtl_bus;

/* ── Userspace ioctl interface ──────────────────────────────────────── */

struct rtl8370mb_smi_msg {
	__u16 reg;
	__u16 val;
};

#define RTL8370MB_IOC_MAGIC	'R'
#define RTL8370MB_SMI_READ	_IOWR(RTL8370MB_IOC_MAGIC, 1, struct rtl8370mb_smi_msg)
#define RTL8370MB_SMI_WRITE	_IOW(RTL8370MB_IOC_MAGIC, 2, struct rtl8370mb_smi_msg)

/* ── SMI read/write (atomic — holds mdio_lock for full sequence) ───── */

static int smi_read(struct mii_bus *bus, u32 reg, u32 *val)
{
	int ret, retry;

	for (retry = 0; retry < SMI_RETRIES; retry++) {
		mutex_lock(&bus->mdio_lock);

		ret = bus->write(bus, SMI_PHY_ADDR, SMI_CTRL0_REG, SMI_ADDR_OP);
		if (ret)
			goto next;
		ret = bus->write(bus, SMI_PHY_ADDR, SMI_ADDRESS_REG, reg);
		if (ret)
			goto next;
		ret = bus->write(bus, SMI_PHY_ADDR, SMI_CTRL1_REG, SMI_READ_OP);
		if (ret)
			goto next;
		ret = bus->read(bus, SMI_PHY_ADDR, SMI_DATA_READ_REG);
		if (ret >= 0) {
			*val = ret;
			mutex_unlock(&bus->mdio_lock);
			return 0;
		}
next:
		mutex_unlock(&bus->mdio_lock);
		msleep(SMI_RETRY_DELAY_MS);
	}

	return ret < 0 ? ret : -ETIMEDOUT;
}

static int smi_write(struct mii_bus *bus, u32 reg, u32 val)
{
	int ret, retry;

	for (retry = 0; retry < SMI_RETRIES; retry++) {
		mutex_lock(&bus->mdio_lock);

		ret = bus->write(bus, SMI_PHY_ADDR, SMI_CTRL0_REG, SMI_ADDR_OP);
		if (ret)
			goto next;
		ret = bus->write(bus, SMI_PHY_ADDR, SMI_ADDRESS_REG, reg);
		if (ret)
			goto next;
		ret = bus->write(bus, SMI_PHY_ADDR, SMI_DATA_WRITE_REG, val);
		if (ret)
			goto next;
		ret = bus->write(bus, SMI_PHY_ADDR, SMI_CTRL1_REG, SMI_WRITE_OP);
		if (ret == 0) {
			mutex_unlock(&bus->mdio_lock);
			return 0;
		}
next:
		mutex_unlock(&bus->mdio_lock);
		msleep(SMI_RETRY_DELAY_MS);
	}

	return ret ? ret : -ETIMEDOUT;
}

static int smi_update(struct mii_bus *bus, u32 reg, u32 mask, u32 val)
{
	u32 orig;
	int ret;

	ret = smi_read(bus, reg, &orig);
	if (ret)
		return ret;
	return smi_write(bus, reg, (orig & ~mask) | (val & mask));
}

/* ── chardev ioctl ──────────────────────────────────────────────────── */

static long rtl8370mb_ioctl(struct file *file, unsigned int cmd,
			     unsigned long arg)
{
	struct rtl8370mb_smi_msg msg;
	u32 val;
	int ret;

	if (!rtl_bus)
		return -ENODEV;

	if (copy_from_user(&msg, (void __user *)arg, sizeof(msg)))
		return -EFAULT;

	switch (cmd) {
	case RTL8370MB_SMI_READ:
		ret = smi_read(rtl_bus, msg.reg, &val);
		if (ret)
			return ret;
		msg.val = val & 0xFFFF;
		if (copy_to_user((void __user *)arg, &msg, sizeof(msg)))
			return -EFAULT;
		return 0;

	case RTL8370MB_SMI_WRITE:
		return smi_write(rtl_bus, msg.reg, msg.val);

	default:
		return -ENOTTY;
	}
}

static const struct file_operations rtl8370mb_fops = {
	.owner		= THIS_MODULE,
	.unlocked_ioctl	= rtl8370mb_ioctl,
	.compat_ioctl	= rtl8370mb_ioctl,
};

static struct miscdevice rtl8370mb_misc = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "rtl8370mb",
	.fops	= &rtl8370mb_fops,
};

/* ── Module init ─────────────────────────────────────────────────────── */

static int __init rtl8370mb_module_init(void)
{
	struct mii_bus *bus;
	u32 chip_id = 0, chip_ver = 0;
	int ret, port;

	bus = alpine_shared_mdio_bus;
	if (!bus) {
		pr_info("rtl8370mb: MDIO bus not available, deferring\n");
		return -EPROBE_DEFER;
	}

	pr_info("rtl8370mb: probing on %s, SMI PHY addr 0x%x\n",
		bus->id, SMI_PHY_ADDR);

	/* Unlock */
	ret = smi_write(bus, MAGIC_REG, MAGIC_VALUE);
	if (ret) {
		pr_err("rtl8370mb: unlock failed: %d\n", ret);
		return ret;
	}

	/* Detect */
	ret = smi_read(bus, CHIP_ID_REG, &chip_id);
	if (ret) {
		pr_err("rtl8370mb: read chip ID failed: %d\n", ret);
		goto lock;
	}
	smi_read(bus, CHIP_VER_REG, &chip_ver);

	pr_info("rtl8370mb: chip_id=0x%04x ver=0x%04x\n", chip_id, chip_ver);

	if (chip_id != 0x6368 && chip_id != 0x0801 && chip_id != 0x6367) {
		pr_err("rtl8370mb: unexpected chip ID 0x%04x\n", chip_id);
		ret = -ENODEV;
		goto lock;
	}

	/* ── EXT1 = RGMII (confirmed from stock Recovery boot) ── */
	ret = smi_update(bus, DI_SELECT_REG0, 0x00F0, EXT_MODE_RGMII << 4);
	if (ret) {
		pr_err("rtl8370mb: set EXT1 RGMII failed: %d\n", ret);
		goto lock;
	}

	/* ── EXT1 RGMII delays ── */
	smi_write(bus, EXT_RGMXF_REG1, RGMXF_TXDELAY | RGMXF_RXDELAY_1);

	/* ── Force EXT1: 1000M, full duplex, link up ── */
	ret = smi_write(bus, DI_FORCE_REG1,
			FORCE_EN | FORCE_LINK | FORCE_DUPLEX | FORCE_SPEED_1000M);
	if (ret) {
		pr_err("rtl8370mb: force EXT1 link failed: %d\n", ret);
		goto lock;
	}

	/* ── Disable EXT0 ── */
	smi_update(bus, DI_SELECT_REG0, 0x000F, EXT_MODE_DISABLE);

	/* ── Port isolation: all LAN + CPU can reach each other ── */
	for (port = 0; port < NUM_PORTS; port++) {
		u32 mask;
		if (port >= NUM_PHY_PORTS && port != EXT1_PORT)
			mask = 0;
		else
			mask = ALL_PORTS_MASK & ~(1 << port);
		smi_write(bus, PORT_ISO_BASE + port, mask);
	}

	/* ── STP forwarding on all active ports ── */
	{
		u32 stp_val = 0;
		for (port = 0; port < 8; port++)
			stp_val |= STP_FORWARDING << (port * 2);
		smi_write(bus, MSTI_CTRL_BASE, stp_val);
		smi_write(bus, MSTI_CTRL_BASE + 1, STP_FORWARDING << 2);
	}

	/* ── MAC learning ── */
	for (port = 0; port < NUM_PHY_PORTS; port++)
		smi_write(bus, LUT_LEARN_LIMIT_BASE + port, LEARN_LIMIT_MAX);
	smi_write(bus, LUT_LEARN_LIMIT_BASE + EXT1_PORT, LEARN_LIMIT_MAX);

	/* ── Max frame size ── */
	smi_write(bus, CFG0_MAX_LEN_REG, CFG0_MAX_LEN_MAX);

	/* ── CPU port: EXT1, no tagging ── */
	smi_write(bus, CPU_PORT_MASK_REG, 1 << EXT1_PORT);
	smi_write(bus, CPU_CTRL_REG,
		  CPU_CTRL_EN | CPU_CTRL_INSERTMODE_NONE | CPU_CTRL_TRAP_PORT_1);

	pr_info("rtl8370mb: initialized — 8 LAN ports bridged to EXT1 (RGMII 1000M)\n");

	/* Register chardev for userspace SMI access */
	rtl_bus = bus;
	ret = misc_register(&rtl8370mb_misc);
	if (ret) {
		pr_err("rtl8370mb: misc_register failed: %d\n", ret);
		rtl_bus = NULL;
	} else {
		pr_info("rtl8370mb: /dev/rtl8370mb ready\n");
	}

	smi_write(bus, MAGIC_REG, 0x0000);
	return 0;

lock:
	smi_write(bus, MAGIC_REG, 0x0000);
	return ret;
}

static void __exit rtl8370mb_module_exit(void)
{
	misc_deregister(&rtl8370mb_misc);
	rtl_bus = NULL;
	pr_info("rtl8370mb: unloaded\n");
}

module_init(rtl8370mb_module_init);
module_exit(rtl8370mb_module_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("RTL8370MB switch init + userspace SMI for UDM Pro");
MODULE_AUTHOR("secfirst");
