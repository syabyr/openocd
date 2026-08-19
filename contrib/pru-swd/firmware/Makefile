# PRU SWD firmware build (host side, TI PRU CGT)
#
# The tilde in CGT=~/Downloads/... is NOT expanded by make's recipe
# shell (it only expands ~ at the start of a word), and a CGT set on
# the make command line overrides assignments below - so the
# normalized value lives in CGT_ABS, derived from CGT either way.
CGT ?= $(HOME)/Downloads/ti-cgt-pru_2.3.3
CGT_ABS := $(patsubst ~/%,$(HOME)/%,$(CGT))

ifeq ($(wildcard $(CGT_ABS)/bin/clpru),)
$(error TI PRU CGT not found at $(CGT_ABS) - run: make CGT=/path/to/ti-cgt-pru_2.x.y)
endif

CLPRU  = $(CGT_ABS)/bin/clpru
HEXPRU = $(CGT_ABS)/bin/hexpru

all: pru-swd.bin

pru-swd.obj: pru-swd.c
	$(CLPRU) --silicon_version=3 --c99 -O2 --gcc --include_path=$(CGT_ABS)/include -c $< -o $@

pru-swd.out: pru-swd.obj am335x_pru0.cmd
	$(CLPRU) -z $< $(CGT_ABS)/lib/rtspruv3_le.lib --rom_model -i$(CGT_ABS)/lib -o $@ am335x_pru0.cmd

pru-swd.bin: pru-swd.out
	$(HEXPRU) -b $< -o $@

clean:
	rm -f pru-swd.obj pru-swd.out pru-swd.bin pru-swd.map

.PHONY: all clean
