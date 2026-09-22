# Out-of-tree build of the patched cdc_ncm for a development machine. See
# docs/nodes/carplay.md "The kernel patch" for how to use it; on a Yocto
# build the patch goes into the kernel recipe instead.
obj-m := cdc_ncm.o
KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
