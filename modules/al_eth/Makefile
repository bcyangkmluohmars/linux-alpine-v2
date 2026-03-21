# Makefile for Annapurna Labs al_eth out-of-tree kernel module
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
obj-m := al_eth.o

# Object files that make up the module
al_eth-y := al_eth_main.o \
            al_eth_sysfs.o \
            al_eth_group_lm.o \
            al_hal_eth_kr.o \
            al_hal_eth_main.o \
            al_hal_iofic.o \
            al_hal_serdes_25g.o \
            al_hal_serdes_hssp.o \
            al_hal_udma_config.o \
            al_hal_udma_debug.o \
            al_hal_udma_iofic.o \
            al_hal_udma_main.o \
            al_init_eth_kr.o \
            al_init_eth_lm.o \
            al_serdes.o \
            alpine_group_lm.o \
            alpine_serdes.o \
            kcompat.o

# Extra CFLAGS for the module
ccflags-y += -I$(src)

.PHONY: all clean modules_install

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

modules_install:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules_install
