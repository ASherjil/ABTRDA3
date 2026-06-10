# gem_uio kernel module build
#
# Native build (on the Zynq device itself):
#   make KDIR=/lib/modules/$(uname -r)/build
#
# Cross-compile (from x86_64 host with FECOS sysroot):
#   make KDIR=/acc/sys/cdk/linux-headers/<release>-aarch64 \
#        ARCH=arm64 \
#        CROSS_COMPILE=/acc/sys/cdk/debian/12/aarch64/sysroots/host/usr/bin/aarch64-linux-gnu-
#
# After building, the .ko's vermagic must match the running kernel exactly,
# or insmod refuses to load.  If the running kernel is built from a git tree
# one commit ahead of the headers tag (e.g. fecos03-1-g8f9a3dc vs fecos03),
# patch the vermagic in the .modinfo section before deploying.

obj-m := gem_uio.o

KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

.PHONY: all clean
