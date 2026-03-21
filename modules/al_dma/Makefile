# Makefile for Annapurna Labs al_dma out-of-tree kernel module
# Target: Linux 6.12 LTS
#
# Usage:
#   make KDIR=/path/to/kernel/build
#   make KDIR=/path/to/kernel/build ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
#
# For the Alpine V2 SoC (aarch64):
#   make KDIR=/path/to/linux-6.12-build ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-

# Kernel build directory (override with KDIR= on command line)
KDIR ?= /lib/modules/$(shell uname -r)/build

# Module name
obj-m := al_dma.o

# Object files that make up the module
# Driver core
al_dma-y := al_dma_drv.o \
            al_dma_sysfs.o

# HAL: SSM (Security/Storage/Memory) layer
al_dma-y += al_hal_ssm.o \
            al_hal_ssm_raid.o \
            al_hal_m2m_udma.o

# HAL: UDMA (Unified DMA) infrastructure - shared with al_eth
al_dma-y += al_hal_udma_main.o \
            al_hal_udma_config.o \
            al_hal_udma_debug.o

# Extra CFLAGS for the module
ccflags-y += -I$(src)
ccflags-y += -DAL_DMA_REVISION=2

.PHONY: all clean modules_install

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

modules_install:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules_install
