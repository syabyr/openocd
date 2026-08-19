// SPDX-License-Identifier: GPL-2.0-or-later

/***************************************************************************
 *   SWD adapter driving the PRU-ICSS of a TI AM335x (BeagleBone)          *
 *                                                                         *
 *   Ported from NIIBE Yutaka's 2016 bbg-swd OpenOCD patch and PRU         *
 *   firmware (Flying Stone Technology, GPL-2.0-or-later).  The original   *
 *   used libprussdrv/uio_pruss; this version works with the modern        *
 *   remoteproc-based kernel PRU stack: PRUSS registers are mapped         *
 *   through /dev/gpiomem (whitelisted mmap helper module), firmware is    *
 *   written straight into PRU0 IRAM, and the mailbox handshake is         *
 *   polling based (command word + completion counter in PRU0 DRAM).       *
 *                                                                         *
 *   Pins are fixed by the PRU firmware (pinned in mode 7 by the kernel    *
 *   pinmux hog):                                                          *
 *     P8_11 = GPIO1_13  SWDIO                                              *
 *     P8_12 = GPIO1_12  SWCLK                                              *
 *     P8_15 = GPIO1_15  nRST (optional)                                    *
 ***************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <jtag/adapter.h>
#include <jtag/interface.h>
#include <jtag/commands.h>
#include <jtag/swd.h>
#include <transport/transport.h>

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

/* AM335x PRU-ICSS (TRM ch.2 memory map + am33xx-l4.dtsi) */
#define PRUSS0_BASE		0x4A300000u
#define PRUSS0_MAP_SIZE		0x00040000u
#define PRU0_DRAM_OFF		0x0000
#define PRU0_CTRL_OFF		0x22000		/* PRU0 control regs */
#define PRU0_IRAM_OFF		0x34000		/* PRU0 instruction RAM */
#define PRU0_IRAM_SIZE		0x2000
#define PRU_CTRL_CTRL		0x0
#define PRU_CTRL_SOFT_RST_N	(1u << 0)
#define PRU_CTRL_EN		(1u << 1)

/* PRU0 DRAM mailbox, word indices (matches firmware pru-swd.c) */
#define MBOX_CMD	0	/* [7:0]=CMD [15:8]=cmd8/parity [23:16]=parity [31:24]=idle */
#define MBOX_DATA	1	/* words 1..8: write value / idle count / bit pattern */
#define MBOX_RESULT	16	/* word 16/17: results */
#define MBOX_COUNTER	18	/* completion counter */
#define MBOX_DELAY	20	/* half-phase delay iterations */
#define MBOX_MAGIC	21	/* protocol magic written by v2 firmware */

enum {
	CMD_HALT = 0,
	CMD_BLINK,
	CMD_GPIO_OUT,
	CMD_GPIO_IN,
	CMD_SIG_IDLE,
	CMD_SIG_GEN,
	CMD_READ_REG,
	CMD_WRITE_REG,
};

/* full-byte commands (the v2 firmware decodes all 8 bits of word 0) */
#define CMD_READ_BLOCK	0x09
#define PRU_PROTO_V2	0x53574432u	/* "SWD2": block read support */
#define PRU_BLOCK_OFF	0x400u		/* DRAM offset of the block buffer */
#define PRU_BLOCK_MAX	1024u		/* words per batch (buffer holds 1792) */

/* PRU firmware timing, measured on a BeagleBone Black with prupoke's
 * timing mode (5000-clock averages): one SWCLK period is ~440 ns with
 * zero half_phase() delay iterations (back-to-back OCP GPIO writes
 * pipeline that fast) and grows by ~160-190 ns per iteration.  The
 * speed mapping picks the nearest tier at or below the requested
 * period; the real rate can land up to ~15% above the request because
 * the iteration granularity is coarse.  Requests of 1800 kHz and up
 * run at the iters=0 OCP-paced floor, ~2.3 MHz measured - that is the
 * practical ceiling for bit-banging SWCLK through GPIO1 over OCP. */
#define PRU_DELAY_NS_PER_ITER		190u
#define PRU_PHASE_FIXED_NS		222u
#define PRU_OCP_FLOOR_NS		550u
#define PRU_MAX_KHZ		2000u

/* Busy-poll iterations before pru_swd_exec falls back to usleep(100).
 * One spin iteration is one uncached mmap read (~150 ns), so this
 * covers several milliseconds - more than any single transaction even
 * at 100 kHz. */
#define PRU_EXEC_SPIN		50000u

/* default firmware location on the target (OpenWrt convention) */
#define PRU_FW_DEFAULT_PATH	"/lib/firmware/pru-swd.bin"
static char *pru_fw_path;		/* NULL = use default */

static int dev_mem_fd = -1;
static volatile uint32_t *pruss_map;		/* PRUSS window */
static volatile uint32_t *pru0_dram;		/* PRU0 DRAM mailbox */
static volatile uint32_t *pru0_ctrl;
static volatile uint32_t *gpio1_map;		/* GPIO1 regs, save/restore */
static uint32_t saved_gpio_oe, saved_gpio_dataout;
static const uint32_t swd_pin_mask = (1u << 13) | (1u << 12) | (1u << 15);

static int queued_retval;

/* v2 firmware protocol: AP DRW reads can be replayed in one mailbox
 * command (CMD_READ_BLOCK).  fw_block_read is set at init from the
 * magic word the firmware writes at boot; with a v1 firmware the
 * driver silently stays on the single-word path. */
static int fw_block_read;
static uint32_t *pend_value[PRU_BLOCK_MAX];
static unsigned int pend_count;
static uint32_t pend_cmd8, pend_delay;

static inline uint32_t pru_ctrl_read(uint32_t offset)
{
	return pru0_ctrl[offset / 4];
}

static inline void pru_ctrl_write(uint32_t offset, uint32_t val)
{
	pru0_ctrl[offset / 4] = val;
}

static int pru_swd_exec(uint32_t w0, unsigned int timeout_ms)
{
	volatile uint32_t *mbox = pru0_dram;
	uint32_t counter0 = mbox[MBOX_COUNTER];
	unsigned int polls = timeout_ms * 10;
	unsigned int spin = PRU_EXEC_SPIN;

	/* command word goes in last: parameter words are already set */
	mbox[MBOX_CMD] = w0;

	for (;;) {
		if (mbox[MBOX_COUNTER] != counter0)
			return ERROR_OK;

		/* A register transaction is a few dozen SWCLK cycles - tens of
		 * microseconds.  Poll the counter directly for that window
		 * before falling back to sleeping: a fixed usleep(100) per
		 * poll added ~100us to every single word transfer and halved
		 * the effective throughput of block reads. */
		if (spin) {
			spin--;
			continue;
		}
		if (polls-- == 0) {
			LOG_ERROR("PRU did not complete command 0x%08x within %u ms "
				"(is the PRUSS clock on?  try: "
				"echo on > /sys/devices/platform/ocp/4a000000.interconnect/"
				"4a000000.interconnect:segment@0/4a326000.target-module/power/control)",
				w0, timeout_ms);
			return ERROR_FAIL;
		}
		usleep(100);
	}
}

/* Execute buffered AP DRW reads as one CMD_READ_BLOCK: the PRU replays
 * the read transaction `count' times without a mailbox round trip per
 * word, stores the data at DRAM 0x400 and reports how many words
 * completed.  Callers must invoke this *before* writing any other
 * mailbox parameter words - it uses MBOX_DATA itself. */
static void pru_swd_flush_pending_reads(void)
{
	if (!pend_count)
		return;

	unsigned int count = pend_count;

	pend_count = 0;		/* first: so nothing below can re-enter */

	pru0_dram[MBOX_DATA] = count;
	int retval = pru_swd_exec(CMD_READ_BLOCK | (pend_cmd8 << 8)
			| (pend_delay << 24), 5000);
	if (retval != ERROR_OK) {
		queued_retval = ERROR_FAIL;
		return;
	}

	uint32_t res = pru0_dram[MBOX_RESULT];
	uint32_t ack = res & 0x7;
	unsigned int done = pru0_dram[MBOX_RESULT + 1];
	if (done > count)
		done = count;

	for (unsigned int i = 0; i < done; i++)
		if (pend_value[i])
			*pend_value[i] = pruss_map[PRU_BLOCK_OFF / 4 + i];

	if (ack != SWD_ACK_OK || (res & 0x8)) {
		LOG_DEBUG("block read stopped after %u/%u words: ack=%u%s",
			  done, count, ack, (res & 0x8) ? ", parity error" : "");
		if (res & 0x8)
			queued_retval = ERROR_FAIL;
		else if (ack == SWD_ACK_FAULT)
			queued_retval = ERROR_SWD_FAULT;
		else if (ack == SWD_ACK_WAIT)
			queued_retval = ERROR_WAIT;
		else
			queued_retval = ERROR_SWD_FAIL;
	}
}

static void pru_swd_idle(unsigned int count)
{
	pru_swd_flush_pending_reads();
	pru0_dram[MBOX_DATA] = count;
	pru_swd_exec(CMD_SIG_IDLE, 100);
}

static int pru_swd_load_firmware(void)
{
	const char *fw_path = pru_fw_path ? pru_fw_path : PRU_FW_DEFAULT_PATH;
	FILE *fw = fopen(fw_path, "rb");
	if (!fw) {
		LOG_ERROR("cannot open PRU firmware %s: %s", fw_path, strerror(errno));
		return ERROR_FAIL;
	}

	static uint32_t image[PRU0_IRAM_SIZE / 4];
	size_t len = fread(image, 1, sizeof(image), fw);
	fclose(fw);
	if (len == 0 || (len & 3)) {
		LOG_ERROR("bad PRU firmware image %s (%zu bytes)", fw_path, len);
		return ERROR_FAIL;
	}
	LOG_INFO("pru-swd: loading firmware %s (%zu bytes) into PRU0 IRAM", fw_path, len);

	/* hold PRU0 in reset while replacing its program */
	pru_ctrl_write(PRU_CTRL_CTRL, 0);

	size_t words = len / 4;
	for (size_t i = 0; i < words; i++)
		pruss_map[PRU0_IRAM_OFF / 4 + i] = image[i];

	/* clear the protocol magic so a stale value from an earlier
	 * firmware session cannot fake block-read support */
	pru0_dram[MBOX_MAGIC] = 0;

	/* release reset and enable; firmware entry point is 0 */
	pru_ctrl_write(PRU_CTRL_CTRL, PRU_CTRL_SOFT_RST_N | PRU_CTRL_EN);
	return ERROR_OK;
}

static int pru_swd_init(void)
{
	LOG_INFO("pru-swd: SWD via PRU-ICSS (P8_11=SWDIO P8_12=SWCLK P8_15=nRST)");

	if (transport_is_swd() && !pruss_map) {
		dev_mem_fd = open("/dev/gpiomem", O_RDWR | O_SYNC);
		if (dev_mem_fd < 0) {
			LOG_ERROR("cannot open /dev/gpiomem: %s (load the gpiomem module)",
				strerror(errno));
			return ERROR_JTAG_INIT_FAILED;
		}

		pruss_map = mmap(NULL, PRUSS0_MAP_SIZE, PROT_READ | PROT_WRITE,
				MAP_SHARED, dev_mem_fd, PRUSS0_BASE);
		gpio1_map = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
				MAP_SHARED, dev_mem_fd, 0x4804C000u);
		if (pruss_map == MAP_FAILED || gpio1_map == MAP_FAILED) {
			LOG_ERROR("mmap PRUSS/GPIO failed: %s (gpiomem too old? PRUSS "
				"region 0x4A300000 must be whitelisted)", strerror(errno));
			close(dev_mem_fd);
			dev_mem_fd = -1;
			return ERROR_JTAG_INIT_FAILED;
		}

		pru0_dram = &pruss_map[PRU0_DRAM_OFF / 4];
		pru0_ctrl = &pruss_map[PRU0_CTRL_OFF / 4];

		saved_gpio_oe = gpio1_map[0x34 / 4];
		saved_gpio_dataout = gpio1_map[0x13C / 4];

		int retval = pru_swd_load_firmware();
		if (retval != ERROR_OK) {
			munmap((void *)pruss_map, PRUSS0_MAP_SIZE);
			munmap((void *)gpio1_map, 4096);
			close(dev_mem_fd);
			dev_mem_fd = -1;
			pruss_map = NULL;
			return ERROR_JTAG_INIT_FAILED;
		}

		/* conservative half-phase delay until "adapter speed" sets it */
		pru0_dram[MBOX_DELAY] = 100;

		/* handshake: one dummy command proves the PRU is alive */
		retval = pru_swd_exec(CMD_GPIO_IN, 500);
		if (retval != ERROR_OK) {
			LOG_ERROR("PRU firmware handshake failed (remoteproc must be "
				"offline for PRU0: cat /sys/class/remoteproc/remoteproc0/state)");
			return ERROR_JTAG_INIT_FAILED;
		}
		LOG_INFO("pru-swd: PRU0 firmware up, mailbox handshake ok");

		fw_block_read = (pru0_dram[MBOX_MAGIC] == PRU_PROTO_V2);
		LOG_INFO("pru-swd: firmware protocol v2: %s",
			fw_block_read ? "block reads enabled" : "single-word reads only");
	}

	return ERROR_OK;
}

static int pru_swd_quit(void)
{
	pend_count = 0;		/* drop buffered reads: session is over */
	if (pruss_map) {
		/* ask firmware to stop touching the pins, then disable the PRU.
		 * The PRU polls for a *non-zero* command word, so HALT (=0)
		 * must be sent as 8: low 3 bits still select the HALT case. */
		pru_swd_exec(CMD_HALT | 0x8, 100);
		pru_ctrl_write(PRU_CTRL_CTRL, 0);

		/* restore pin state we found before the firmware took over */
		gpio1_map[0x13C / 4] = (gpio1_map[0x13C / 4] & ~swd_pin_mask)
				| (saved_gpio_dataout & swd_pin_mask);
		gpio1_map[0x34 / 4] = (gpio1_map[0x34 / 4] & ~swd_pin_mask)
				| (saved_gpio_oe & swd_pin_mask);

		munmap((void *)pruss_map, PRUSS0_MAP_SIZE);
		munmap((void *)gpio1_map, 4096);
		pruss_map = NULL;
	}

	if (dev_mem_fd >= 0) {
		close(dev_mem_fd);
		dev_mem_fd = -1;
	}

	return ERROR_OK;
}

/***************************************************************************
 * SWD transport interface                                                 *
 ***************************************************************************/

static void pru_swd_clear_sticky_errors(void)
{
	queued_retval = ERROR_OK;
	pru_swd_flush_pending_reads();
	pru0_dram[MBOX_DATA] = STKCMPCLR | STKERRCLR | WDERRCLR | ORUNERRCLR;
	pru_swd_exec(CMD_WRITE_REG
			| ((uint32_t)swd_cmd(false, false, DP_ABORT) << 8), 100);
}

static int pru_swd_switch_seq(enum swd_special_seq seq)
{
	const uint8_t *buf;
	unsigned int len, idle_after = 0;

	pru_swd_flush_pending_reads();

	switch (seq) {
	case LINE_RESET:
		LOG_DEBUG("SWD line reset");
		buf = swd_seq_line_reset;
		len = swd_seq_line_reset_len;
		break;
	case JTAG_TO_SWD:
		LOG_DEBUG("JTAG-to-SWD");
		buf = swd_seq_jtag_to_swd;
		len = swd_seq_jtag_to_swd_len;
		idle_after = 8;
		break;
	case SWD_TO_JTAG:
		LOG_DEBUG("SWD-to-JTAG");
		buf = swd_seq_swd_to_jtag;
		len = swd_seq_swd_to_jtag_len;
		break;
	case SWD_TO_DORMANT:
		LOG_DEBUG("SWD-to-dormant");
		buf = swd_seq_swd_to_dormant;
		len = swd_seq_swd_to_dormant_len;
		break;
	case DORMANT_TO_SWD:
		LOG_DEBUG("dormant-to-SWD");
		buf = swd_seq_dormant_to_swd;
		len = swd_seq_dormant_to_swd_len;
		break;
	case JTAG_TO_DORMANT:
		LOG_DEBUG("JTAG-to-dormant");
		buf = swd_seq_jtag_to_dormant;
		len = swd_seq_jtag_to_dormant_len;
		break;
	case DORMANT_TO_JTAG:
		LOG_DEBUG("dormant-to-JTAG");
		buf = swd_seq_dormant_to_jtag;
		len = swd_seq_dormant_to_jtag_len;
		break;
	default:
		LOG_ERROR("sequence %d not supported", seq);
		return ERROR_FAIL;
	}

	if (len > 256) {
		LOG_ERROR("sequence %u bits too long for PRU mailbox", len);
		return ERROR_FAIL;
	}

	memcpy((void *)&pru0_dram[MBOX_DATA], buf, (len + 7) / 8);
	int retval = pru_swd_exec(CMD_SIG_GEN | (len << 8), 1000);
	if (retval != ERROR_OK)
		return retval;

	if (idle_after)
		pru_swd_idle(idle_after);

	return ERROR_OK;
}

static void pru_swd_read_reg(uint8_t cmd, uint32_t *value, uint32_t ap_delay_hint)
{
	assert(cmd & SWD_CMD_RNW);
	assert(ap_delay_hint < 256);

	if (queued_retval != ERROR_OK) {
		LOG_DEBUG("skip read_reg, queued_retval=%d", queued_retval);
		return;
	}

	uint32_t cmd8 = cmd | SWD_CMD_START | SWD_CMD_PARK;
	uint32_t delay = (cmd & SWD_CMD_APNDP) ? ap_delay_hint : 0;

	/* AP DRW reads come in long runs (mem-ap block reads rely on TAR
	 * auto-increment).  Buffer them and replay with a single mailbox
	 * command: the per-word round trip dominated read throughput. */
	if (fw_block_read && (cmd & SWD_CMD_APNDP)
			&& (cmd & SWD_CMD_A32) == SWD_CMD_A32) {
		if (pend_count && pend_cmd8 != cmd8)
			pru_swd_flush_pending_reads();
		if (!pend_count) {
			pend_cmd8 = cmd8;
			pend_delay = delay;
		}
		pend_value[pend_count++] = value;
		if (pend_count == PRU_BLOCK_MAX)
			pru_swd_flush_pending_reads();
		return;
	}

	pru_swd_flush_pending_reads();

	for (unsigned int retries = 128; ; retries--) {
		int retval = pru_swd_exec(CMD_READ_REG | (cmd8 << 8) | (delay << 24), 1000);
		if (retval != ERROR_OK) {
			queued_retval = ERROR_FAIL;
			return;
		}

		uint32_t res = pru0_dram[MBOX_RESULT];
		uint32_t data = pru0_dram[MBOX_RESULT + 1];
		int ack = res & 0x7;
		int parity = (res >> 31) & 0x1;

		LOG_DEBUG("%s %s reg %X = %08x",
			  ack == SWD_ACK_OK ? "OK" :
			  ack == SWD_ACK_WAIT ? "WAIT" :
			  ack == SWD_ACK_FAULT ? "FAULT" : "JUNK",
			  cmd & SWD_CMD_APNDP ? "AP" : "DP",
			  (cmd & SWD_CMD_A32) >> 1, (unsigned)data);

		switch (ack) {
		case SWD_ACK_OK:
			if (parity != parity_u32(data)) {
				LOG_DEBUG("wrong parity detected");
				queued_retval = ERROR_FAIL;
				return;
			}
			if (value)
				*value = data;
			return;
		case SWD_ACK_WAIT:
			LOG_DEBUG("SWD ACK WAIT");
			if (retries == 0) {
				queued_retval = ERROR_WAIT;
				return;
			}
			pru_swd_clear_sticky_errors();
			break;
		case SWD_ACK_FAULT:
			LOG_DEBUG("SWD ACK FAULT");
			queued_retval = ERROR_SWD_FAULT;
			return;
		default:
			LOG_DEBUG("no valid acknowledge: ack=%d", ack);
			queued_retval = ERROR_SWD_FAIL;
			return;
		}
	}
}

static void pru_swd_write_reg(uint8_t cmd, uint32_t value, uint32_t ap_delay_hint)
{
	assert(!(cmd & SWD_CMD_RNW));
	assert(ap_delay_hint < 256);

	if (queued_retval != ERROR_OK) {
		LOG_DEBUG("skip write_reg, queued_retval=%d", queued_retval);
		return;
	}

	pru_swd_flush_pending_reads();

	for (unsigned int retries = 128; ; retries--) {
		uint32_t delay = (cmd & SWD_CMD_APNDP) ? ap_delay_hint : 0;
		uint32_t cmd8 = cmd | SWD_CMD_START | SWD_CMD_PARK;
		uint32_t parity = parity_u32(value);

		pru0_dram[MBOX_DATA] = value;
		pru0_dram[MBOX_CMD] = 0;
		int retval = pru_swd_exec(CMD_WRITE_REG | (cmd8 << 8)
				| (parity << 16) | (delay << 24), 1000);
		if (retval != ERROR_OK) {
			queued_retval = ERROR_FAIL;
			return;
		}

		int ack = pru0_dram[MBOX_RESULT] & 0x7;

		LOG_DEBUG("%s %s reg %X <- %08x",
			  ack == SWD_ACK_OK ? "OK" :
			  ack == SWD_ACK_WAIT ? "WAIT" :
			  ack == SWD_ACK_FAULT ? "FAULT" : "JUNK",
			  cmd & SWD_CMD_APNDP ? "AP" : "DP",
			  (cmd & SWD_CMD_A32) >> 1, (unsigned)value);

		switch (ack) {
		case SWD_ACK_OK:
			return;
		case SWD_ACK_WAIT:
			LOG_DEBUG("SWD ACK WAIT");
			if (retries == 0) {
				queued_retval = ERROR_WAIT;
				return;
			}
			pru_swd_clear_sticky_errors();
			break;
		case SWD_ACK_FAULT:
			LOG_DEBUG("SWD ACK FAULT");
			queued_retval = ERROR_SWD_FAULT;
			return;
		default:
			LOG_DEBUG("no valid acknowledge: ack=%d", ack);
			queued_retval = ERROR_SWD_FAIL;
			return;
		}
	}
}

static int pru_swd_run_queue(void)
{
	pru_swd_flush_pending_reads();
	pru_swd_idle(8);
	int retval = queued_retval;
	queued_retval = ERROR_OK;
	LOG_DEBUG("SWD queue return value: %d", retval);
	return retval;
}

static const struct swd_driver pru_swd_swd = {
	.init = pru_swd_init,			/* invoked via adapter init */
	.switch_seq = pru_swd_switch_seq,
	.read_reg = pru_swd_read_reg,
	.write_reg = pru_swd_write_reg,
	.run = pru_swd_run_queue,
};

/***************************************************************************
 * Adapter driver plumbing                                                 *
 ***************************************************************************/

static void pru_swd_execute_reset(struct jtag_command *cmd)
{
	/* srst wire on P8_15 (GPIO1_15), active low */
	uint32_t assert = cmd->cmd.reset->srst ? 1 : 0;

	pru0_dram[MBOX_DATA] = 1u << 15;
	pru0_dram[MBOX_DATA + 1] = !assert;
	pru_swd_exec(CMD_GPIO_OUT, 100);
}

static void pru_swd_execute_sleep(struct jtag_command *cmd)
{
	jtag_sleep(cmd->cmd.sleep->us);
}

static void pru_swd_execute_command(struct jtag_command *cmd)
{
	switch (cmd->type) {
	case JTAG_RESET:
		pru_swd_execute_reset(cmd);
		break;
	case JTAG_SLEEP:
		pru_swd_execute_sleep(cmd);
		break;
	default:
		LOG_ERROR("BUG: unknown JTAG command type encountered");
		exit(-1);
	}
}

static int pru_swd_execute_queue(struct jtag_command *cmd_queue)
{
	struct jtag_command *cmd = cmd_queue;

	pru_swd_flush_pending_reads();
	while (cmd) {
		pru_swd_execute_command(cmd);
		cmd = cmd->next;
	}

	return ERROR_OK;
}

static int pru_swd_speed(int speed)
{
	/* speed is in kHz (see pru_swd_khz); translate to delay iterations */
	if (pru0_dram) {
		uint32_t period_ns = 1000000u / (uint32_t)speed;
		uint32_t iters = period_ns <= PRU_OCP_FLOOR_NS ? 0u
			: (period_ns - PRU_PHASE_FIXED_NS) / PRU_DELAY_NS_PER_ITER;

		pru0_dram[MBOX_DELAY] = iters;
		LOG_INFO("pru-swd: SWCLK half-phase delay %u iterations (~%d kHz)",
			iters, speed);
	}
	return ERROR_OK;
}

static int pru_swd_khz(int khz, int *speed)
{
	if (!khz) {
		LOG_ERROR("RCLK not supported");
		return ERROR_FAIL;
	}
	if (khz > (int)PRU_MAX_KHZ) {
		LOG_WARNING("pru-swd clamped to %u kHz", PRU_MAX_KHZ);
		khz = PRU_MAX_KHZ;
	}
	*speed = khz;
	return ERROR_OK;
}

static int pru_swd_speed_div(int speed, int *khz)
{
	*khz = speed;
	return ERROR_OK;
}

static int pru_swd_reset(int trst, int srst)
{
	(void)trst;		/* no TRST wire for SWD */
	if (!pru0_dram)
		return ERROR_OK;

	pru0_dram[MBOX_DATA] = 1u << 15;
	pru0_dram[MBOX_DATA + 1] = !srst;	/* nRST is active low */
	return pru_swd_exec(CMD_GPIO_OUT, 100);
}

COMMAND_HANDLER(pru_swd_handle_firmware_command)
{
	if (CMD_ARGC != 1)
		return ERROR_COMMAND_SYNTAX_ERROR;

	free(pru_fw_path);
	pru_fw_path = strdup(CMD_ARGV[0]);
	return ERROR_OK;
}

static const struct command_registration pru_swd_subcommand_handlers[] = {
	{
		.name = "firmware",
		.handler = pru_swd_handle_firmware_command,
		.mode = COMMAND_CONFIG,
		.help = "PRU firmware image path (raw binary for PRU0 IRAM).",
		.usage = "<path>",
	},
	COMMAND_REGISTRATION_DONE
};

static const struct command_registration pru_swd_command_handlers[] = {
	{
		.name = "pruswd",
		.mode = COMMAND_ANY,
		.help = "pru-swd adapter commands",
		.chain = pru_swd_subcommand_handlers,
		.usage = "",
	},
	COMMAND_REGISTRATION_DONE
};

static struct jtag_interface pru_swd_interface = {
	.supported = DEBUG_CAP_TMS_SEQ,
	.execute_queue = pru_swd_execute_queue,
};

struct adapter_driver pru_swd_adapter_driver = {
	.name = "pruswd",
	.transport_ids = TRANSPORT_SWD,
	.transport_preferred_id = TRANSPORT_SWD,
	.commands = pru_swd_command_handlers,

	.init = pru_swd_init,
	.quit = pru_swd_quit,
	.reset = pru_swd_reset,
	.speed = pru_swd_speed,
	.khz = pru_swd_khz,
	.speed_div = pru_swd_speed_div,

	.jtag_ops = &pru_swd_interface,		/* minimal: sleep/reset only */
	.swd_ops = &pru_swd_swd,
};
