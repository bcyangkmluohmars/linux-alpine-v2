# Makefile for Annapurna Labs Alpine V2 SSM crypto engine out-of-tree kernel module
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
obj-m := al_ssm.o

# Object files that make up the module
# -- Linux PCI driver + crypto framework glue --
al_ssm-y := al_ssm_main.o

# -- HAL: SSM (Security Services Module) --
al_ssm-y += al_hal_ssm.o
al_ssm-y += al_hal_ssm_crypto.o
al_ssm-y += al_hal_ssm_crypto_cfg.o
al_ssm-y += al_hal_ssm_crc_memcpy.o
al_ssm-y += al_hal_ssm_raid.o

# -- HAL: UDMA (Unified DMA) --
al_ssm-y += al_hal_udma_main.o
al_ssm-y += al_hal_udma_config.o
al_ssm-y += al_hal_udma_debug.o
al_ssm-y += al_hal_udma_iofic.o
al_ssm-y += al_hal_m2m_udma.o

# -- HAL: I/O fabric --
al_ssm-y += al_hal_iofic.o
al_ssm-y += al_hal_unit_adapter.o

# Extra CFLAGS for the module
# -I$(src) : find all headers in our flat directory
# -DAL_HAVE_TYPES : prevent HAL from redefining standard types (already in <linux/types.h>)
# -DAL_UDMA_EX : enable extended UDMA register definitions (needed for V3/V4 structs to compile)
ccflags-y += -I$(src)
ccflags-y += -DAL_HAVE_TYPES
ccflags-y += -DAL_UDMA_EX

.PHONY: all clean modules_install

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

modules_install:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules_install
