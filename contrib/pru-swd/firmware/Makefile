# PRU SWD firmware build (host side, TI CGT pru 2.3.3)
CGT ?= $(HOME)/Downloads/ti-cgt-pru_2.3.3
CLPRU  = $(CGT)/bin/clpru
HEXPRU = $(CGT)/bin/hexpru

all: pru-swd.bin

pru-swd.obj: pru-swd.c
	$(CLPRU) --silicon_version=3 --c99 -O2 --gcc --include_path=$(CGT)/include -c $< -o $@

pru-swd.out: pru-swd.obj am335x_pru0.cmd
	$(CLPRU) -z $< $(CGT)/lib/rtspruv3_le.lib --rom_model -i$(CGT)/lib -o $@ am335x_pru0.cmd

pru-swd.bin: pru-swd.out
	$(HEXPRU) -b $< -o $@

clean:
	rm -f pru-swd.obj pru-swd.out pru-swd.bin pru-swd.map

.PHONY: all clean
