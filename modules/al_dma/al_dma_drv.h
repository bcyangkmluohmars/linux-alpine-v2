/* SPDX-License-Identifier: GPL-2.0 */
/*
 * al_dma_drv.h - Annapurna Labs DMA Engine Driver internal header
 */

#ifndef __AL_DMA_DRV_H__
#define __AL_DMA_DRV_H__

#include <linux/pci.h>
#include <linux/dmaengine.h>
#include <linux/spinlock.h>
#include <linux/interrupt.h>

#include "al_hal_ssm.h"
#include "al_hal_ssm_raid.h"

#define AL_DMA_MAX_XOR		31

/* Software descriptor for tracking in-flight operations */
struct al_dma_sw_desc {
	struct dma_async_tx_descriptor	txd;
	struct list_head		node;
	int				tx_descs_count;
	bool				is_memset;
};

/* Per-channel data */
struct al_dma_chan {
	struct dma_chan			chan;
	struct al_dma_device		*device;
	int				idx;

	spinlock_t			lock;
	struct tasklet_struct		cleanup_task;

	/* Descriptor management */
	struct al_dma_sw_desc		*sw_ring;
	int				sw_ring_count;
	int				head;
	int				tail;

	/* HAL queue resources */
	union al_udma_desc		*tx_ring;
	dma_addr_t			tx_ring_dma;
	uint8_t				*tx_cring;
	dma_addr_t			tx_cring_dma;

	union al_udma_desc		*rx_ring;
	dma_addr_t			rx_ring_dma;
	uint8_t				*rx_cring;
	dma_addr_t			rx_cring_dma;

	unsigned long			completed_cookie;
};

/* Per-device data */
struct al_dma_device {
	struct pci_dev			*pdev;
	struct dma_device		dma_dev;

	void __iomem			*udma_regs;     /* BAR 0 */
	void __iomem			*app_regs;      /* BAR 4 */

	struct al_ssm_dma		hal_dma;
	uint8_t				rev_id;

	int				num_channels;
	struct al_dma_chan		channels[DMA_MAX_Q];
};

/* Sysfs */
int al_dma_sysfs_init(struct al_dma_device *dev);
void al_dma_sysfs_remove(struct al_dma_device *dev);

#endif /* __AL_DMA_DRV_H__ */
