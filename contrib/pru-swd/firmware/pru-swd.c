// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * pru-swd.c — SWD protocol engine on PRU0, BeagleBone (AM335x PRU-ICSS)
 *
 * C port of NIIBE Yutaka's BBG-SWD pru-swd.p (pasm, 2016, Flying Stone
 * Technology).  Signal timing (GPIO OE based SWDIO turnaround, clock
 * phases, bit order) mirrors the original assembly; the mailbox
 * handshake is reworked:
 *
 *   original: ARM kicks INTC event 21 to wake SLPing PRU, PRU notifies
 *             ARM via INTC event 19 (needs uio_pruss/libprussdrv).
 *   this:     PRU polls the command word in PRU0 DRAM; completion is
 *             signalled by bumping the counter word.  No INTC setup,
 *             no uio_pruss — coexists with the remoteproc PRU stack
 *             (firmware is loaded straight into IRAM by OpenOCD).
 *
 * Pins (GPIO mode 7, muxed by the kernel pinmux hog):
 *   P8_11 = GPIO1_13  SWDIO
 *   P8_12 = GPIO1_12  SWCLK
 *   P8_15 = GPIO1_15  nRST  (host side only, via CMD_GPIO_OUT)
 *
 * Mailbox in PRU0 DRAM, word indices (word0 = byte offset 0):
 *   0      command: [7:0]=CMD [15:8]=cmd8/SIG length [23:16]=write parity
 *          [31:24]=idle clocks after AP access
 *          PRU writes 0 as soon as it consumes the command (host sees ACK)
 *   1..8   parameters: write value / idle count / SIG_GEN bit pattern
 *   16     result 0 (READ: parity<<31|ack, WRITE: ack, GPIO_IN: DATAIN)
 *   17     result 1 (READ: data word)
 *   18     completion counter (host snapshots, waits for it to change)
 *   20     half-phase delay loop iterations, host writable, fw default 100
 */

#include <stdint.h>

/* PRU local data space addresses (AM335x, PRU core view) */
#define PRUSS_CFG_SYSCFG	0x00026004u	/* CT_PRUCFG + SYSCFG */
#define STANDBY_INIT		(1u << 4)	/* 1 = disable OCP master */

#define GPIO1_BASE		0x4804C100u	/* GPIO1 register window */
#define GPIO_OE			(0x34 / 4)
#define GPIO_DATAIN		(0x38 / 4)
#define GPIO_CLEARDATAOUT	(0x90 / 4)
#define GPIO_SETDATAOUT		(0x94 / 4)

#define SWDIO_BIT	13		/* P8_11, GPIO1_13 */
#define SWCLK_BIT	12		/* P8_12, GPIO1_12 */
#define SWDIO_MASK	(1u << SWDIO_BIT)
#define SWCLK_MASK	(1u << SWCLK_BIT)

#define MBOX		((volatile uint32_t *)0x00000000u) /* PRU0 DRAM */
#define MBOX_CMD	0
#define MBOX_DATA	1
#define MBOX_RESULT	16
#define MBOX_COUNTER	18
#define MBOX_DELAY	20

enum {
	CMD_HALT = 0,
	CMD_BLINK,			/* reserved: no-op */
	CMD_GPIO_OUT,			/* word1 = mask, word2 = level */
	CMD_GPIO_IN,			/* result = GPIO1 DATAIN */
	CMD_SIG_IDLE,			/* word1 = idle clock count */
	CMD_SIG_GEN,			/* byte1 = bit count, words1..8 = pattern */
	CMD_READ_REG,
	CMD_WRITE_REG,
};

static volatile uint32_t * const gpio1 = (volatile uint32_t *)GPIO1_BASE;

static inline void dio_drive(uint32_t mask, int hi)
{
	gpio1[hi ? GPIO_SETDATAOUT : GPIO_CLEARDATAOUT] = mask;
}

static inline void clk_low(void)  { dio_drive(SWCLK_MASK, 0); }
static inline void clk_high(void) { dio_drive(SWCLK_MASK, 1); }

static inline uint32_t dio_sample(void)
{
	return gpio1[GPIO_DATAIN] & SWDIO_MASK;
}

static inline void dio_set_output(void)
{
	gpio1[GPIO_OE] &= ~SWDIO_MASK;	/* 0 = output */
}

static inline void dio_set_input(void)
{
	gpio1[GPIO_OE] |= SWDIO_MASK;	/* 1 = input */
}

/* One SWCLK half phase.  Iteration count is host controlled (mailbox
 * word 20) so the host can trade speed for margin without reflashing.
 * Loop body costs a few PRU cycles per iteration; the host "adapter
 * speed" mapping is calibrated conservatively.
 */
static inline void half_phase(void)
{
	volatile uint32_t n = MBOX[MBOX_DELAY];

	if (!n)
		n = 100;		/* safe fallback */
	while (n--)
		__delay_cycles(1);
}

static void write_bit(uint32_t bit)
{
	clk_low();
	dio_drive(SWDIO_MASK, bit != 0);
	half_phase();
	clk_high();			/* target samples on rising edge */
	half_phase();
}

static uint32_t read_bit(void)
{
	clk_low();
	half_phase();
	uint32_t b = dio_sample();
	clk_high();
	half_phase();
	return b;
}

/* Turnaround: release SWDIO while clock is low, one clock */
static void trn_input(void)
{
	clk_low();
	dio_set_input();
	half_phase();
	clk_high();
	half_phase();
}

static void sig_idle_cycles(uint32_t count)
{
	while (count--) {
		clk_low();
		half_phase();
		clk_high();
		half_phase();
	}
}

static void cmd_sig_gen(uint32_t w0)
{
	uint32_t n = (w0 >> 8) & 0xff;
	const uint8_t *pat = (const uint8_t *)&MBOX[MBOX_DATA];

	dio_set_output();
	for (uint32_t i = 0; i < n; i++) {
		clk_low();
		dio_drive(SWDIO_MASK, (pat[i >> 3] >> (i & 7)) & 1);
		half_phase();
		clk_high();
		half_phase();
	}
	dio_drive(SWDIO_MASK, 1);
	MBOX[MBOX_RESULT] = 0;
}

static void cmd_read_reg(uint32_t w0)
{
	uint32_t cmd8 = (w0 >> 8) & 0xff;
	uint32_t idle = (w0 >> 24) & 0xff;
	uint32_t ack = 0, data = 0;

	for (uint32_t i = 0; i < 8; i++)	/* request, LSB first */
		write_bit((cmd8 >> i) & 1);

	trn_input();

	for (uint32_t i = 0; i < 3; i++)	/* ACK, LSB first */
		ack |= read_bit() ? (1u << i) : 0;
	for (uint32_t i = 0; i < 32; i++)	/* RDATA, LSB first */
		data |= read_bit() ? (1u << i) : 0;
	uint32_t parity = read_bit();

	/* TRN back to host */
	clk_low();
	half_phase();
	clk_high();
	half_phase();

	if (idle) {				/* ap_delay idle clocks */
		dio_drive(SWDIO_MASK, 0);
		dio_set_output();
		sig_idle_cycles(idle);
	}
	dio_drive(SWDIO_MASK, 1);
	dio_set_output();

	MBOX[MBOX_RESULT] = (parity ? (1u << 31) : 0) | (ack & 7);
	MBOX[MBOX_RESULT + 1] = data;
}

static void cmd_write_reg(uint32_t w0)
{
	uint32_t cmd8 = (w0 >> 8) & 0xff;
	uint32_t dparity = (w0 >> 16) & 0xff;
	uint32_t idle = (w0 >> 24) & 0xff;
	uint32_t val = MBOX[MBOX_DATA];
	uint32_t ack = 0;

	for (uint32_t i = 0; i < 8; i++)	/* request, LSB first */
		write_bit((cmd8 >> i) & 1);

	trn_input();

	for (uint32_t i = 0; i < 3; i++)	/* ACK, LSB first */
		ack |= read_bit() ? (1u << i) : 0;

	/* TRN + first WDATA bit fused: drive while clock is low */
	clk_low();
	half_phase();
	dio_drive(SWDIO_MASK, val & 1);
	dio_set_output();
	half_phase();
	clk_high();
	half_phase();

	for (uint32_t i = 1; i < 32; i++)
		write_bit((val >> i) & 1);
	write_bit(dparity & 1);			/* WDATA parity */

	if (idle) {
		dio_drive(SWDIO_MASK, 0);
		sig_idle_cycles(idle);
	}
	dio_drive(SWDIO_MASK, 1);

	MBOX[MBOX_RESULT] = ack & 7;
}

static void cmd_gpio_out(void)
{
	uint32_t mask = MBOX[MBOX_DATA];
	uint32_t val = MBOX[MBOX_DATA + 1];

	gpio1[val ? GPIO_SETDATAOUT : GPIO_CLEARDATAOUT] = mask;
	gpio1[GPIO_OE] &= ~mask;		/* ensure output */
	MBOX[MBOX_RESULT] = 0;
}

void main(void)
{
	/* Enable OCP master port so PRU can reach GPIO1 */
	*(volatile uint32_t *)PRUSS_CFG_SYSCFG &= ~STANDBY_INIT;

	/* Idle both lines high, then enable their output drivers */
	dio_drive(SWDIO_MASK, 1);
	dio_drive(SWCLK_MASK, 1);
	gpio1[GPIO_OE] &= ~(SWDIO_MASK | SWCLK_MASK);

	MBOX[MBOX_CMD] = 0;
	MBOX[MBOX_RESULT] = 0;
	MBOX[MBOX_RESULT + 1] = 0;
	MBOX[MBOX_COUNTER] = 0;

	for (;;) {
		uint32_t w0;

		while ((w0 = MBOX[MBOX_CMD]) == 0)
			;			/* wait for command */

		MBOX[MBOX_CMD] = 0;		/* consumed — host ACK */

		switch (w0 & 7) {
		case CMD_HALT:
			MBOX[MBOX_RESULT] = 0;
			MBOX[MBOX_COUNTER]++;
			__halt();		/* host clears CTRL.EN after */
			return;
		case CMD_BLINK:
			MBOX[MBOX_RESULT] = 0;
			break;
		case CMD_GPIO_OUT:
			cmd_gpio_out();
			break;
		case CMD_GPIO_IN:
			MBOX[MBOX_RESULT] = gpio1[GPIO_DATAIN];
			break;
		case CMD_SIG_IDLE: {
			uint32_t n = MBOX[MBOX_DATA];

			dio_drive(SWDIO_MASK, 0);	/* park low */
			sig_idle_cycles(n);
			dio_drive(SWDIO_MASK, 1);
			MBOX[MBOX_RESULT] = 0;
			break;
		}
		case CMD_SIG_GEN:
			cmd_sig_gen(w0);
			break;
		case CMD_READ_REG:
			cmd_read_reg(w0);
			break;
		case CMD_WRITE_REG:
			cmd_write_reg(w0);
			break;
		}

		MBOX[MBOX_COUNTER]++;		/* completion signal */
	}
}
