// SPDX-License-Identifier: GPL-2.0
/*
 * Annapurna Labs Alpine SGPO (Serial GPIO Output) driver
 *
 * The SGPO is a shift-register based GPIO output controller found in the
 * Alpine V2/V3 PBS (Platform Bus System) block.  It serializes up to 64
 * output pins onto a small number of data lines (DS[3:0]) clocked by SHCP
 * and latched by STCP.  Typical use: SATA activity/presence LEDs.
 *
 * Register map reverse-engineered from the Annapurna Labs HAL
 * (drivers/pbs/al_hal_sgpo.c, dual-licensed GPL-2.0).
 *
 * Copyright (C) 2026 secfirst contributors
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/io.h>
#include <linux/gpio/driver.h>
#include <linux/spinlock.h>
#include <linux/clk.h>

/* ---------------------------------------------------------------------------
 * Register definitions (from al_hal_sgpo_{common,pergroup}_regs.h)
 * ---------------------------------------------------------------------------
 *
 * Overall layout (offsets from SGPO base):
 *
 *   0x0000  Common block         (0x1000 bytes incl. padding)
 *   0x1000  Per-group block 0    (0x1000 bytes incl. padding)
 *   0x2000  Per-group block 1
 *   ...
 *   Alpine V2: 4 groups  (pins  0-31)
 *   Alpine V3: 6 groups  (pins  0-47)
 *   Max:       8 groups  (pins  0-63)
 */

#define AL_SGPO_PINS_PER_GROUP		8
#define AL_SGPO_NUM_GROUPS_V2		4
#define AL_SGPO_MAX_GROUPS		8
#define AL_SGPO_MAX_PINS		(AL_SGPO_MAX_GROUPS * AL_SGPO_PINS_PER_GROUP)

/* ---- Common registers (base + 0x0000) ---------------------------------- */
#define AL_SGPO_COMMON_CONTROL		0x0000
#define AL_SGPO_COMMON_CPU_VDD		0x0004
#define AL_SGPO_COMMON_OUT_EN_SGPO	0x0008
#define AL_SGPO_COMMON_OUT_EN_CPU_VDD	0x000c
#define AL_SGPO_COMMON_SWAP_LEDS	0x0010
#define AL_SGPO_COMMON_INUSE_HIGH	0x0014
#define AL_SGPO_COMMON_INUSE_LOW	0x0018

/* control register fields */
#define CTRL_GROUPS_MASK		0x00000003
#define CTRL_GROUPS_SHIFT		0
#define CTRL_SGI_CLEAR			BIT(2)
#define CTRL_SGI_ENABLE			BIT(3)
#define CTRL_CPU_VDD_SEL		BIT(5)
#define CTRL_CPU_VDD_EN			BIT(7)
#define CTRL_SATA_LED_SEL_MASK		0x00000300
#define CTRL_SATA_LED_SEL_SHIFT		8
#define CTRL_SDI_SETUP_MASK		0x0000f000
#define CTRL_SDI_SETUP_SHIFT		12
#define CTRL_CLK_FREQ_MASK		0x000f0000
#define CTRL_CLK_FREQ_SHIFT		16
#define CTRL_CNTR_SCALE_MASK		0x00f00000
#define CTRL_CNTR_SCALE_SHIFT		20
#define CTRL_CNTR_SCALE_NORMAL		(0xd << CTRL_CNTR_SCALE_SHIFT)
#define CTRL_UPDATE_FREQ_MASK		0x1f000000
#define CTRL_UPDATE_FREQ_SHIFT		24

/* out_en_sgpo register fields */
#define OUT_EN_SHCP			BIT(0)
#define OUT_EN_STCP			BIT(1)
#define OUT_EN_DS_MASK			0x0000003c  /* DS[3:0] */

/* ---- Per-group registers (base + 0x1000 + group * 0x1000) --------------- */
#define AL_SGPO_GROUP_SIZE		0x1000
#define AL_SGPO_GROUP_BASE		0x1000

/* Per-group config (offset within group block) */
#define GRP_RATES			0x0000
#define GRP_MODE			0x0004
#define GRP_INVERT			0x0008
#define GRP_BLINK			0x000c
#define GRP_STRETCH			0x0010

/* Per-group config vectors (offset 0x400 within group block).
 *
 * The hardware uses an unusual addressing scheme: to set pin N within a
 * group, you write to conf_vec[1 << N].val with the value shifted left by N.
 * To read pin N, read conf_vec[1 << N].val and shift right by N.
 * To read all 8 pins at once, read conf_vec[0xFF].val.
 *
 * Each conf_vec entry is a single uint32_t (4 bytes).
 */
#define GRP_VEC_BASE			0x0400
#define GRP_VEC(idx)			(GRP_VEC_BASE + (idx) * 4)
#define GRP_VEC_ALL			GRP_VEC(0xff)

/* Pin mode: 2 bits per pin in the mode register.
 *   0x0 = HW (driven by ETH/SATA hardware)
 *   0x3 = USER (driven by software via conf_vec)
 */
#define PIN_MODE_HW			0x0
#define PIN_MODE_USER			0x3

/* ---------------------------------------------------------------------------
 * Driver data
 * ---------------------------------------------------------------------------
 */

struct al_sgpo {
	struct gpio_chip gc;
	void __iomem *regs;
	spinlock_t lock;        /* protects register access */
	unsigned int num_groups;
	unsigned int ref_clk_freq;
};

/* ---------------------------------------------------------------------------
 * Register helpers
 * ---------------------------------------------------------------------------
 */

static inline void __iomem *al_sgpo_group_reg(struct al_sgpo *sgpo,
					       unsigned int group,
					       unsigned int offset)
{
	return sgpo->regs + AL_SGPO_GROUP_BASE +
	       (group * AL_SGPO_GROUP_SIZE) + offset;
}

static inline void al_sgpo_rmw(void __iomem *reg, u32 mask, u32 val)
{
	u32 tmp = readl(reg);

	writel((tmp & ~mask) | (val & mask), reg);
}

/* ---------------------------------------------------------------------------
 * gpio_chip callbacks
 * ---------------------------------------------------------------------------
 */

static int al_sgpo_get_direction(struct gpio_chip *gc, unsigned int offset)
{
	/* All SGPO pins are outputs only */
	return GPIO_LINE_DIRECTION_OUT;
}

static int al_sgpo_direction_output(struct gpio_chip *gc, unsigned int offset,
				    int value)
{
	struct al_sgpo *sgpo = gpiochip_get_data(gc);
	unsigned int group = offset / AL_SGPO_PINS_PER_GROUP;
	unsigned int bit = offset % AL_SGPO_PINS_PER_GROUP;
	unsigned long flags;

	if (group >= sgpo->num_groups)
		return -EINVAL;

	spin_lock_irqsave(&sgpo->lock, flags);

	/* Switch pin to USER mode (2 bits per pin in mode register) */
	al_sgpo_rmw(al_sgpo_group_reg(sgpo, group, GRP_MODE),
		    0x3 << (bit * 2),
		    PIN_MODE_USER << (bit * 2));

	/* Set the value via the conf_vec indexed write trick:
	 *   conf_vec[1 << bit].val = value << bit
	 */
	writel(value ? (1 << bit) : 0,
	       al_sgpo_group_reg(sgpo, group, GRP_VEC(1 << bit)));

	spin_unlock_irqrestore(&sgpo->lock, flags);

	return 0;
}

static void al_sgpo_set(struct gpio_chip *gc, unsigned int offset, int value)
{
	struct al_sgpo *sgpo = gpiochip_get_data(gc);
	unsigned int group = offset / AL_SGPO_PINS_PER_GROUP;
	unsigned int bit = offset % AL_SGPO_PINS_PER_GROUP;
	unsigned long flags;

	if (group >= sgpo->num_groups)
		return;

	spin_lock_irqsave(&sgpo->lock, flags);

	writel(value ? (1 << bit) : 0,
	       al_sgpo_group_reg(sgpo, group, GRP_VEC(1 << bit)));

	spin_unlock_irqrestore(&sgpo->lock, flags);
}

static int al_sgpo_get(struct gpio_chip *gc, unsigned int offset)
{
	struct al_sgpo *sgpo = gpiochip_get_data(gc);
	unsigned int group = offset / AL_SGPO_PINS_PER_GROUP;
	unsigned int bit = offset % AL_SGPO_PINS_PER_GROUP;
	u32 val;

	if (group >= sgpo->num_groups)
		return -EINVAL;

	/* Read back the last written value from conf_vec[1 << bit] */
	val = readl(al_sgpo_group_reg(sgpo, group, GRP_VEC(1 << bit)));

	return (val >> bit) & 1;
}

static int al_sgpo_direction_input(struct gpio_chip *gc, unsigned int offset)
{
	/* SGPO is output only */
	return -ENOTSUPP;
}

/* ---------------------------------------------------------------------------
 * Hardware initialisation
 * ---------------------------------------------------------------------------
 *
 * Mirrors al_sgpo_hw_init() from the HAL with blink_base_rate = NORMAL.
 * After this the shift-register serialiser is running and pins can be
 * toggled from software via the conf_vec mechanism.
 */

static void al_sgpo_hw_init(struct al_sgpo *sgpo)
{
	void __iomem *ctrl = sgpo->regs + AL_SGPO_COMMON_CONTROL;
	void __iomem *out_en = sgpo->regs + AL_SGPO_COMMON_OUT_EN_SGPO;
	void __iomem *out_en_vdd = sgpo->regs + AL_SGPO_COMMON_OUT_EN_CPU_VDD;

	/* Set blink base rate to NORMAL (0xd) */
	al_sgpo_rmw(ctrl, CTRL_CNTR_SCALE_MASK, CTRL_CNTR_SCALE_NORMAL);

	/* Enable all output drivers: SHCP, STCP, DS[3:0] */
	al_sgpo_rmw(out_en,
		    OUT_EN_SHCP | OUT_EN_STCP | OUT_EN_DS_MASK,
		    OUT_EN_SHCP | OUT_EN_STCP | OUT_EN_DS_MASK);

	/* Enable CPU VDD control outputs (needed for serialiser) */
	al_sgpo_rmw(out_en_vdd, 0x7, 0x7);

	/* Enable CPU VDD serial mode */
	al_sgpo_rmw(ctrl, CTRL_CPU_VDD_EN, CTRL_CPU_VDD_EN);

	/* Enable the SGI (serial GPIO interface) block */
	al_sgpo_rmw(ctrl, CTRL_SGI_ENABLE, CTRL_SGI_ENABLE);
}

/* ---------------------------------------------------------------------------
 * Platform driver
 * ---------------------------------------------------------------------------
 */

static int al_sgpo_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct al_sgpo *sgpo;
	unsigned int npins;
	int ret;

	sgpo = devm_kzalloc(dev, sizeof(*sgpo), GFP_KERNEL);
	if (!sgpo)
		return -ENOMEM;

	sgpo->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(sgpo->regs))
		return PTR_ERR(sgpo->regs);

	spin_lock_init(&sgpo->lock);

	/* Alpine V2 has 4 groups of 8 pins = 32 pins.
	 * Allow DTS override via "ngpios" property.
	 */
	sgpo->num_groups = AL_SGPO_NUM_GROUPS_V2;

	if (of_property_read_u32(dev->of_node, "ngpios", &npins) == 0) {
		unsigned int groups = DIV_ROUND_UP(npins, AL_SGPO_PINS_PER_GROUP);

		if (groups > AL_SGPO_MAX_GROUPS) {
			dev_warn(dev, "ngpios %u exceeds max (%u), clamping\n",
				 npins, AL_SGPO_MAX_PINS);
			groups = AL_SGPO_MAX_GROUPS;
		}
		sgpo->num_groups = groups;
	}

	npins = sgpo->num_groups * AL_SGPO_PINS_PER_GROUP;

	/* Initialise the hardware serialiser */
	al_sgpo_hw_init(sgpo);

	/* Register GPIO chip */
	sgpo->gc.label = dev_name(dev);
	sgpo->gc.parent = dev;
	sgpo->gc.owner = THIS_MODULE;
	sgpo->gc.base = -1;   /* dynamic assignment */
	sgpo->gc.ngpio = npins;
	sgpo->gc.get_direction = al_sgpo_get_direction;
	sgpo->gc.direction_input = al_sgpo_direction_input;
	sgpo->gc.direction_output = al_sgpo_direction_output;
	sgpo->gc.set = al_sgpo_set;
	sgpo->gc.get = al_sgpo_get;
	sgpo->gc.can_sleep = false;
	sgpo->gc.parent = dev;

	ret = devm_gpiochip_add_data(dev, &sgpo->gc, sgpo);
	if (ret) {
		dev_err(dev, "failed to add GPIO chip: %d\n", ret);
		return ret;
	}

	dev_info(dev, "Alpine SGPO driver probed. %u pins (%u groups)\n",
		 npins, sgpo->num_groups);

	return 0;
}

static const struct of_device_id al_sgpo_of_match[] = {
	{ .compatible = "annapurna-labs,alpine-sgpo" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, al_sgpo_of_match);

static struct platform_driver al_sgpo_driver = {
	.probe = al_sgpo_probe,
	.driver = {
		.name = "al-sgpo",
		.of_match_table = al_sgpo_of_match,
	},
};
module_platform_driver(al_sgpo_driver);

MODULE_DESCRIPTION("Annapurna Labs Alpine SGPO (Serial GPIO Output) driver");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("secfirst contributors");
