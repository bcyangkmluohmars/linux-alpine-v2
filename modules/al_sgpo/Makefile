# Makefile for Annapurna Labs SGPO (Serial GPIO Output) out-of-tree kernel module
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
obj-m := al_sgpo.o

# Extra CFLAGS for the module
ccflags-y += -I$(src)

.PHONY: all clean modules_install

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

modules_install:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules_install
