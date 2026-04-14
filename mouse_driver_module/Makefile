obj-m += mouse_input_filter.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

ui:
	python3 mouse_filter_dashboard.py

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean