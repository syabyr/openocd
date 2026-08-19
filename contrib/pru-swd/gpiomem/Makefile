# gpiomem out-of-tree build.
#
# Native build (on the BeagleBone, with kernel headers/obj installed):
#   make
# Cross build:
#   make KDIR=/path/to/target-linux-tree TOOLCHAIN=/path/to/prefix/arm-linux-
#
# A leading ~ in KDIR/TOOLCHAIN is tolerated (normalized below), even
# though make's recipe shell does not expand it mid-word; variables
# set on the make command line override assignments, so the
# normalized values live in separate variables.
KDIR ?= /lib/modules/$(shell uname -r)/build
TOOLCHAIN ?=

KDIR_ABS := $(patsubst ~/%,$(HOME)/%,$(KDIR))
TOOLCHAIN_ABS := $(patsubst ~/%,$(HOME)/%,$(TOOLCHAIN))

ifeq ($(wildcard $(KDIR_ABS)/Makefile),)
$(error kernel tree not found at $(KDIR_ABS) - run: make KDIR=/path/to/linux)
endif

obj-m += gpiomem.o

# ARM either way (the module is AM335x-specific): native on a
# BeagleBone, or cross with TOOLCHAIN set.
ARCH ?= arm

all:
	$(MAKE) -C $(KDIR_ABS) M=$(CURDIR) ARCH=$(ARCH) CROSS_COMPILE=$(TOOLCHAIN_ABS) modules

clean:
	$(MAKE) -C $(KDIR_ABS) M=$(CURDIR) ARCH=$(ARCH) CROSS_COMPILE=$(TOOLCHAIN_ABS) clean

.PHONY: all clean
