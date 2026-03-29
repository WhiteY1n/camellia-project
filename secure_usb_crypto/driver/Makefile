obj-m += usb_crypto_drv.o
ccflags-y += -I$(src)/../include

KDIR := /lib/modules/6.17.0-19-generic/build
PWD := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
