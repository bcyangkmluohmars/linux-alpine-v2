KDIR ?= /lib/modules/$(shell uname -r)/build

obj-m := rtl8370mb_init.o

ccflags-y += -I$(src)

.PHONY: all clean

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
