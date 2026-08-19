/* prupoke - PRU0 bring-up probe over /dev/gpiomem
 *
 * Mirrors the openocd pruswd driver: load pru-swd.bin into PRU0 IRAM,
 * then report PC/CTRL and run a mailbox handshake (CMD_GPIO_IN=3).
 *
 * Timing mode (no target needed):
 *   prupoke t <iters> <clocks>
 * times CMD_SIG_IDLE of <clocks> clocks at half-phase delay <iters>
 * and one 46-clock read transaction, and prints ns/clock.  Used to
 * calibrate the driver's iters = f(adapter speed) mapping.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <errno.h>
#include <stdint.h>
#include <time.h>

#define PRUSS_BASE	0x4A300000u
#define PRUSS_SIZE	0x40000u
#define CTRL_OFF	0x22000
#define IRAM_OFF	0x34000
#define IRAM_SIZE	0x2000
#define SYSCFG_OFF	0x26004

#define MBOX_CMD	0
#define MBOX_DATA	1
#define MBOX_RESULT	16
#define MBOX_COUNTER	18
#define MBOX_DELAY	20

static volatile uint32_t *dram;

static uint32_t exec_cmd(uint32_t w0, const char *what)
{
	uint32_t c0 = dram[MBOX_COUNTER];

	dram[MBOX_CMD] = w0;
	for (int i = 0; i < 5000; i++) {
		if (dram[MBOX_COUNTER] != c0)
			return dram[MBOX_RESULT];
		usleep(1000);
	}
	fprintf(stderr, "PRU did not complete %s (w0=%08x)\n", what, w0);
	exit(1);
}

/* Timing variant: busy-poll.  The usleep(1000) poll above floors any
 * measurement at ~1 ms — with an R30-paced clock a 5000-clock command
 * finishes far below that, so the sleep would completely mask the
 * number being measured. */
static uint32_t exec_cmd_busy(uint32_t w0, const char *what)
{
	uint32_t c0 = dram[MBOX_COUNTER];

	dram[MBOX_CMD] = w0;
	for (long i = 0; i < 200000000L; i++)
		if (dram[MBOX_COUNTER] != c0)
			return dram[MBOX_RESULT];
	fprintf(stderr, "PRU did not complete %s (w0=%08x)\n", what, w0);
	exit(1);
}

static double now_s(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* ---- timing mode ---- */
static int timing_mode(int iters, int clocks)
{
	dram[MBOX_DELAY] = iters;

	/* warm-up + settle (CMD_SIG_IDLE=4, clock count in DATA) */
	dram[MBOX_DATA] = clocks;
	exec_cmd(4, "SIG_IDLE warmup");

	int reps = 20;
	double t0 = now_s();
	for (int i = 0; i < reps; i++) {
		dram[MBOX_DATA] = clocks;
		exec_cmd_busy(4, "SIG_IDLE");
	}
	double dt = now_s() - t0;

	/* one read transaction: 2 idle + 8 req + TRN + 3 ack + 32 data
	 * + parity + TRN = 48 clocks, 36 of them sampling SWDIO */
	t0 = now_s();
	exec_cmd_busy(6 | (0xA5u << 8), "READ_REG");
	double dt_xact = now_s() - t0;

	printf("iters=%d clocks=%d reps=%d\n", iters, clocks, reps);
	printf("idle: %.1f us/cmd -> %.0f ns/clock\n",
		dt / reps * 1e6, dt / reps / clocks * 1e9);
	printf("read xact (48 clocks, 36 sampled): %.1f us -> %.0f ns/clock\n",
		dt_xact * 1e6, dt_xact / 48 * 1e9);
	return 0;
}

int main(int argc, char **argv)
{
	const char *fw_path = (argc > 1 && strcmp(argv[1], "t") != 0)
		? argv[1] : "/lib/firmware/pru-swd.bin";
	int fd = open("/dev/gpiomem", O_RDWR | O_SYNC);
	if (fd < 0) { perror("open /dev/gpiomem"); return 1; }

	volatile uint32_t *pru = mmap(NULL, PRUSS_SIZE, PROT_READ | PROT_WRITE,
			MAP_SHARED, fd, PRUSS_BASE);
	if (pru == MAP_FAILED) { perror("mmap"); return 1; }

	volatile uint32_t *ctrl = &pru[CTRL_OFF / 4];
	volatile uint32_t *iram = &pru[IRAM_OFF / 4];
	volatile uint32_t *syscfg = &pru[SYSCFG_OFF / 4];

	printf("before: CTRL=%08x PC=%08x SYSCFG=%08x\n",
			ctrl[0], ctrl[1], *syscfg);

	/* load firmware */
	static uint32_t image[IRAM_SIZE / 4];
	FILE *f = fopen(fw_path, "rb");
	if (!f) { perror(fw_path); return 1; }
	size_t len = fread(image, 1, sizeof(image), f);
	fclose(f);
	printf("firmware %s: %zu bytes\n", fw_path, len);

	ctrl[0] = 0;			/* hold reset */
	for (size_t i = 0; i < len / 4; i++)
		iram[i] = image[i];
	ctrl[0] = 3;			/* SOFT_RST_N | EN */
	usleep(1000);

	dram = pru;			/* global for exec_cmd/timing_mode */

	if (argc == 4 && strcmp(argv[1], "t") == 0)
		return timing_mode(atoi(argv[2]), atoi(argv[3]));

	printf("after load: CTRL=%08x\n", ctrl[0]);
	for (int i = 0; i < 5; i++) {
		printf("  PC=%08x\n", ctrl[1]);
		usleep(20000);
	}

	/* mailbox handshake: CMD_GPIO_IN */
	uint32_t c0 = dram[18];
	printf("counter before=%u, mbox cmd=%08x\n", c0, dram[0]);
	dram[0] = 3;
	for (int i = 0; i < 500; i++) {
		if (dram[18] != c0) break;
		usleep(1000);
	}
	printf("counter after=%u mbox cmd=%08x result0=%08x delay(w20)=%08x\n",
			dram[18], dram[0], dram[16], dram[20]);
	printf("final PC=%08x SYSCFG=%08x GPCFG0=%08x GPCFG1=%08x\n",
			ctrl[1], *syscfg, pru[0x2612c / 4], pru[0x26130 / 4]);
	return 0;
}
