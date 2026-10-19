KDIR ?= /lib/modules/$(shell uname -r)/build

ifneq ($(KERNELRELEASE),)
obj-m += esp32_link.o
else

.PHONY: all module app overlay clean

all: module app overlay

module:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

app: esp32-ctl

esp32-ctl: esp32_ctl.c esp32_link.h
	$(CC) -std=c11 -O2 -Wall -Wextra -pthread -o $@ esp32_ctl.c

overlay: esp32-link.dtbo

esp32-link.dtbo: esp32-link-overlay.dts
	dtc -@ -I dts -O dtb -o $@ $<

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
	rm -f esp32-ctl esp32-link.dtbo

endif
