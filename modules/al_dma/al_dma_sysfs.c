// SPDX-License-Identifier: GPL-2.0
/*
 * al_dma_sysfs.c - Sysfs interface for Annapurna Labs DMA Engine
 *
 * Exposes DMA engine status and statistics via sysfs.
 */

#include <linux/device.h>
#include <linux/pci.h>
#include <linux/dmaengine.h>

#include "al_dma_drv.h"

static ssize_t al_dma_version_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "1.0.0-k6.12\n");
}

static ssize_t al_dma_channels_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct al_dma_device *aldev = pci_get_drvdata(pdev);

	return sysfs_emit(buf, "%d\n", aldev->num_channels);
}

static ssize_t al_dma_revision_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct al_dma_device *aldev = pci_get_drvdata(pdev);

	return sysfs_emit(buf, "%d\n", aldev->rev_id);
}

static DEVICE_ATTR(dma_version, 0444, al_dma_version_show, NULL);
static DEVICE_ATTR(dma_channels, 0444, al_dma_channels_show, NULL);
static DEVICE_ATTR(dma_revision, 0444, al_dma_revision_show, NULL);

static struct attribute *al_dma_attrs[] = {
	&dev_attr_dma_version.attr,
	&dev_attr_dma_channels.attr,
	&dev_attr_dma_revision.attr,
	NULL
};

static const struct attribute_group al_dma_attr_group = {
	.name = "al_dma",
	.attrs = al_dma_attrs,
};

int al_dma_sysfs_init(struct al_dma_device *dev)
{
	return sysfs_create_group(&dev->pdev->dev.kobj, &al_dma_attr_group);
}

void al_dma_sysfs_remove(struct al_dma_device *dev)
{
	sysfs_remove_group(&dev->pdev->dev.kobj, &al_dma_attr_group);
}
