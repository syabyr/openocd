# out-of-tree build against the kernel tree that matches the running
# kernel (6.12.94, built 2026-06-29; vermagic-relevant config identical
# to ./linux-6.12.94 which has Module.symvers already prepared)
KDIR ?= $(HOME)/develop/openwrt/omap/linux-6.12.94
TOOLCHAIN ?= $(HOME)/develop/openwrt/omap/dev/sdk/staging_dir/toolchain-arm_cortex-a8+vfpv3_gcc-14.3.0_musl_eabi/bin/arm-openwrt-linux-muslgnueabi-

obj-m += gpiomem.o

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) ARCH=arm CROSS_COMPILE=$(TOOLCHAIN) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) ARCH=arm CROSS_COMPILE=$(TOOLCHAIN) clean
