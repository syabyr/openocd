# pruswd — SWD via the AM335x PRU-ICSS (BeagleBone)

`pruswd` is an OpenOCD adapter driver that turns a BeagleBone (Black,
Green, … — any AM335x board with PRU-ICSS and access to the P8 header
pins below) into an SWD debug probe.  A tiny firmware on PRU0 bit-bangs
the SWD lines with cycle-accurate turnaround control, while OpenOCD on
the ARM side talks to it through a shared-memory mailbox.  No USB
adapter, no libprussdrv, no `uio_pruss` — it coexists with the modern
remoteproc PRU stack, and it also runs fine on OpenWrt-style systems
where `/dev/mem` is unavailable.

It is a C port of NIIBE Yutaka's **bbg-swd** (Flying Stone Technology,
2016), reworked for the current OpenOCD adapter driver API and the
remoteproc kernel stack.

```
┌──────────────────────────── AM335x ────────────────────────────┐
│                                                                │
│  OpenOCD (ARM, Linux)              PRU0 (200 MHz, firmware)    │
│  ┌──────────────────────┐          ┌────────────────────────┐  │
│  │ adapter driver       │  mailbox │ SWD engine             │  │
│  │      pruswd         ◄──────────►│ write_bit / read_bit   │  │
│  └─────────┬────────────┘ PRU0     │ SWDIO turnaround via   │  │
│            │              DRAM     │ GPIO1 OE flip over OCP │  │
│            │ /dev/gpiomem          └──────────┬─────────────┘  │
│            │ (whitelist mmap)   IRAM ◄────────┘ firmware load  │
└────────────┼──────────────────────────────────┼────────────────┘
             │                                  │
        P8_11 ┴ SWDIO ◄─────────────────── SWDIO ┘
        P8_12 ┴ SWCLK ───────────────────► SWCLK
        P8_15 ┴ nRST  (optional)          nRST
        P9_1/2 ┴ GND                       GND
```

## Contents

| Path | What it is |
|---|---|
| `firmware/pru-swd.c` | PRU0 firmware source (single C file) |
| `firmware/am335x_pru0.cmd` | TI linker command file |
| `firmware/Makefile` | builds `pru-swd.bin` with TI PRU CGT |
| `gpiomem/gpiomem.c` | kernel module providing `/dev/gpiomem` |
| `gpiomem/Makefile` | out-of-tree module build |
| `prupoke.c` | board bring-up probe (loads firmware, tests mailbox) |
| `../../tcl/interface/pruswd.cfg` | OpenOCD interface config |
| `../../src/jtag/drivers/pru-swd.c` | the OpenOCD adapter driver |

## Pins and wiring

The pins are fixed by the firmware and must be muxed to GPIO (mode 7)
by the kernel pinmux — see [Device tree](#device-tree).

| BeagleBone pin | AM335x | GPIO | SWD signal |
|---|---|---|---|
| P8_11 | GPMC_AD13 | GPIO1_13 | SWDIO |
| P8_12 | GPMC_AD12 | GPIO1_12 | SWCLK |
| P8_15 | GPMC_AD15 | GPIO1_15 | nRST (optional) |
| P9_1 / P9_2 | — | — | GND |

Connect SWDIO, SWCLK, GND (and nRST if you use it) to the target.  The
target must be powered — from its own supply or from a BeagleBone 3V3
pin (P9_3/P9_4); mind the current limit of the BeagleBone's regulator.
The lines are push-pull, **not** 5 V tolerant: target I/O must be 3.3 V.

## How it works

1. **Firmware load.**  On `init` the driver mmaps the PRU-ICSS window
   (`0x4a300000`, 256 KiB) through `/dev/gpiomem`, holds PRU0 in reset
   (`PRU0_CTRL.CTRL = 0`), copies the raw firmware image straight into
   PRU0 IRAM (`+0x34000`), then releases it (`CTRL = SOFT_RST_N|EN`).
   PRU0 always boots at PC 0; the linker script pins the C entry stub
   there.
2. **Mailbox.**  All communication happens through words in PRU0 DRAM
   (offset 0).  The host writes parameters first, the command word
   last; the PRU consumes the command by zeroing it, executes, bumps a
   completion counter; the host polls the counter.  (The HALT command
   is the exception — see the protocol table.)
3. **Bit banging.**  The PRU drives SWDIO/SWCLK through GPIO1
   SETDATAOUT/CLEARDATAOUT/OE registers over the OCP master port (it
   clears `STANDBY_INIT` in the PRUSS SYSCFG first).  SWDIO direction
   turnaround is done by flipping the GPIO OE bit between clock edges,
   exactly like the original bbg-swd.

## Requirements

* BeagleBone (AM3358/AM3356 — the PRU-ICSS must be present; AM3352
  bucks used in some boards have no PRU)
* Kernel with the PRUSS target module enabled (the `pruss`/
  `irq-pruss-intc` modules may be loaded; PRU0 itself must be *unused*
  — `remoteproc0` state `offline`)
* The `gpiomem` module from this directory (loaded, `/dev/gpiomem`
  present).  The stock upstream `gpio-memory`-style modules usually
  map only GPIO banks; this one additionally whitelists the PRUSS
  window which pruswd needs.
* OpenOCD built with `--enable-pruswd` (and `PKG_CONFIG=false` when
  cross-building against a toolchain without target libusb, to keep
  configure from picking up the host's)

## Building

### PRU firmware

Get the TI PRU compiler (CGT), e.g. `ti-cgt-pru_2.3.3` from
ti.com (free download), then:

```sh
cd contrib/pru-swd/firmware
make CGT=~/Downloads/ti-cgt-pru_2.3.3
# → pru-swd.bin (raw image for PRU0 IRAM, entry at 0)
```

Install it where the driver looks for it:

```sh
install -m 0644 pru-swd.bin /lib/firmware/pru-swd.bin
```

> **Do not change the linker script's first section.**  The PRU boots
> at PC 0 unconditionally; `am335x_pru0.cmd` pins `_c_int00` at
> address 0 (`.text:_c_int00* > 0x00000000`).  Dropping that line
> produces a firmware that executes the wrong code from the first
> cycle (symptom: PC frozen, mailbox never answered, `SYSCFG` keeps
> `STANDBY_INIT` set).

### gpiomem kernel module

Build out-of-tree against the *running* kernel, with a cross toolchain
if you build on another machine:

```sh
cd contrib/pru-swd/gpiomem
make KDIR=/path/to/linux-tree \
     TOOLCHAIN=/path/to/toolchain/bin/arm-openwrt-linux-muslgnueabi-
```

Install (OpenWrt-style flat module directory — adapt to taste):

```sh
install -m 0644 gpiomem.ko /lib/modules/$(uname -r)/gpiomem.ko
grep -q gpiomem /lib/modules/$(uname -r)/modules.dep \
	|| echo "gpiomem.ko:" >> /lib/modules/$(uname -r)/modules.dep
echo gpiomem > /etc/modules.d/15-gpiomem   # load at boot (OpenWrt)
modprobe gpiomem                           # or insmod the flat .ko
ls -l /dev/gpiomem                         # should exist, mode 0600
```

The whitelist:

| Region | Size | Used for |
|---|---|---|
| `0x44e07000` | 4 KiB | GPIO0 |
| `0x4804c000` | 4 KiB | GPIO1 (SWDIO/SWCLK/nRST) |
| `0x481ac000` | 4 KiB | GPIO2 |
| `0x481ae000` | 4 KiB | GPIO3 |
| `0x4a300000` | 256 KiB | PRU-ICSS (mailbox, CTRL, IRAM) |

### Device tree

The three pins must be muxed to GPIO mode with the receiver enabled so
the PRU can read SWDIO when it releases the line.  A pinmux hog in the
board DTS is the simplest way (verified on `am335x-boneblack.dts`,
kernel 6.12):

```dts
&am33xx_pinmux {
	pinctrl-0 = <&clkout2_pin &pru_swd_pins>;

	pru_swd_pins: pru-swd-pins {
		pinctrl-single,pins = <
			AM33XX_PADCONF(AM335X_PIN_GPMC_AD13, PIN_INPUT_PULLUP, MUX_MODE7)	/* P8_11 SWDIO */
			AM33XX_PADCONF(AM335X_PIN_GPMC_AD12, PIN_INPUT_PULLUP, MUX_MODE7)	/* P8_12 SWCLK */
			AM33XX_PADCONF(AM335X_PIN_GPMC_AD15, PIN_INPUT_PULLUP, MUX_MODE7)	/* P8_15 nRST  */
		>;
	};
};
```

Rebuild the DTB (`make ti/omap/am335x-boneblack.dtb`) and install it
where your bootloader loads it (BeagleBone Black: the FAT boot
partition, `/dtbs/am335x-boneblack.dtb`).

### OpenOCD

```sh
./bootstrap            # if building from git
./configure --enable-pruswd --enable-internal-jimtcl \
            [--host=... --build=...] [PKG_CONFIG=false]
make
```

## Using with OpenOCD

Minimal (probe the DP only):

```sh
openocd -f interface/pruswd.cfg \
        -c "swd newdap chip dp -expected-id 0" \
        -c "dap create chip.dap -chain-position chip.dp" \
        -c init -c "dap info"
```

Typical with a Cortex-M target (example for an STM32):

```sh
openocd -f interface/pruswd.cfg -f target/stm32f1x.cfg \
        -c "adapter speed 300" -c init -c "targets"
```

Commands provided by the driver:

| Command | Effect |
|---|---|
| `pruswd firmware <path>` | firmware image (default `/lib/firmware/pru-swd.bin`) |

Expected output on a healthy board:

```
Info : pru-swd: SWD via PRU-ICSS (P8_11=SWDIO P8_12=SWCLK P8_15=nRST)
Info : pru-swd: loading firmware /lib/firmware/pru-swd.bin (1400 bytes) into PRU0 IRAM
Info : pru-swd: PRU0 firmware up, mailbox handshake ok
Info : pru-swd: SWCLK half-phase delay 62 iterations (~300 kHz)
Info : clock speed 300 kHz
```

With no target attached (or bad wiring) the DP probe fails — that is
expected: `Error: Error connecting DP: cannot read IDR`.

On exit the driver asks the firmware to stop, disables PRU0 and
restores the GPIO1 OE/DATAOUT bits it changed.

## prupoke — board bring-up

`prupoke.c` is a standalone probe for the moments when OpenOCD is not
yet the problem: it loads the firmware exactly like the driver, then
prints PC/CTRL/SYSCFG and runs one mailbox command.

```sh
cc -O2 -o prupoke prupoke.c        # on the BeagleBone
./prupoke                          # or: ./prupoke /path/to/pru-swd.bin
```

Healthy firmware:

```
after load: CTRL=00008003
  PC=000000bb            ← jumping between two PCs = main poll loop
counter after=1 mbox cmd=00000000 result0=3601b200
final PC=000000bb SYSCFG=0000000a   ← STANDBY_INIT cleared
```

* `counter after=1` and `mbox cmd=0` — mailbox works (command consumed)
* `result0` bit 13 (SWDIO) and bit 12 (SWCLK) set — PRU drives both
  lines high; jumper P8_11 to GND and re-run to watch bit 13 fall
* `SYSCFG` bit 4 cleared — PRU enabled its OCP master port

## Mailbox protocol reference

Words are 32-bit, little endian, in PRU0 DRAM (host view: PRUSS
`+0x0`):

| Word | Direction | Meaning |
|---|---|---|
| 0 | host→PRU | command: `[7:0]` CMD, `[15:8]` cmd8 (register request incl. START/STOP/PARK) or SIG_GEN bit count, `[23:16]` write parity, `[31:24]` idle clocks after AP access.  PRU zeroes this word to acknowledge. |
| 1..8 | host→PRU | parameters: WRITE value (word 1), GPIO_OUT mask/value (words 1/2), SIG_IDLE count (word 1), SIG_GEN bit pattern (256 bits, LSB-first) |
| 16 | PRU→host | result: READ `parity<<31 \| ack`, WRITE `ack`, GPIO_IN raw `GPIO1.DATAIN` |
| 17 | PRU→host | READ: data word |
| 18 | PRU→host | completion counter, incremented after every command |
| 20 | host→PRU | half-phase delay iterations (firmware default 100 ≈ 190 kHz) |

Commands (`word0[7:0]`):

| Code | Command | Notes |
|---|---|---|
| 0 | HALT | writes result, bumps counter, `__halt()`.  **Must be sent as `0x08`** — the PRU polls for a non-zero word, `0x00` means "no command". |
| 1 | BLINK | reserved no-op |
| 2 | GPIO_OUT | words 1/2 = mask, level; also forces OE output |
| 3 | GPIO_IN | result 16 = GPIO1 DATAIN |
| 4 | SIG_IDLE | word 1 = idle clocks, SWDIO parked low |
| 5 | SIG_GEN | byte 1 = bit count (≤256), words 1..8 pattern, LSB first |
| 6 | READ_REG | SWD transaction: 8 request bits, TRN, 3 ack, 32 data, parity, TRN (46 clocks) |
| 7 | WRITE_REG | SWD transaction: 8 request bits, TRN, 3 ack, TRN, 32 data, parity (46 clocks) |

Both register commands are preceded by 2 idle clocks with SWDIO driven
low.  Idle clocks must keep SWDIO **low**: a high clock is taken for the
START bit of a request, shifting the whole transaction (the target then
stays silent and the ack reads 7).  The write turnaround before WDATA is
its own unsampled clock — fusing it with data bit 0 shortens the
transaction to 45 clocks and misaligns every transaction after it,
symptom: the first one or two writes succeed, then ack=7 forever.

## Timing

SWDIO setup/sample happens against the SWCLK rising edge; each half
phase costs `2 × iterations × 25 ns + ~200 ns` of fixed OCP-register
overhead (PRU at 200 MHz, ~5 cycles per delay-loop iteration).  The
driver maps `adapter speed` to iterations as
`(1000000/kHz − 200) / 50`:

| adapter speed | delay iterations |
|---:|---:|
| 1000 kHz | 16 |
| 500 kHz | 36 |
| 300 kHz | 62 |
| 100 kHz | 196 |

1 MHz is the driver's clamp; long flat cables, level shifters or a
target with weak drive may need 300 kHz or below.

## Troubleshooting

| Symptom | Cause / fix |
|---|---|
| `cannot open /dev/gpiomem` | `gpiomem` module not loaded |
| `mmap PRUSS/GPIO failed` | old `gpiomem` without the PRUSS whitelist entry — rebuild the module from `gpiomem/` |
| `PRU did not complete command … within 500 ms` | PRU0 not running: check `cat /sys/class/remoteproc/remoteproc0/state` is `offline`; check the firmware image is the one built from this tree (a firmware linked without `_c_int00` at 0 hangs silently — verify with `prupoke`, PC must move) |
| `Error connecting DP: cannot read IDR` | adapter side fine, no target answered: wiring, target power, target not in SWD mode (JTAG-DP only part), speed too high |
| works, then dies after reboot | the pinmux hog lives in the DTB — make sure the bootloader really loads the updated one from the boot partition |

## Credits & license

* NIIBE Yutaka, *bbg-swd* (Flying Stone Technology, 2016) — the
  original PRU firmware design and OpenOCD patch this port builds on.
* Everything here is GPL-2.0-or-later, like OpenOCD itself.
