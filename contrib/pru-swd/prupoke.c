/* prupoke - PRU0 bring-up probe over /dev/gpiomem
 *
 * Mirrors the openocd pruswd driver: load pru-swd.bin into PRU0 IRAM,
 * then report PC/CTRL and run a mailbox handshake (CMD_GPIO_IN=3).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <errno.h>
#include <stdint.h>

#define PRUSS_BASE	0x4A300000u
#define PRUSS_SIZE	0x40000u
#define CTRL_OFF	0x22000
#define IRAM_OFF	0x34000
#define IRAM_SIZE	0x2000
#define SYSCFG_OFF	0x26004

int main(int argc, char **argv)
{
	const char *fw_path = argc > 1 ? argv[1] : "/lib/firmware/pru-swd.bin";
	int fd = open("/dev/gpiomem", O_RDWR | O_SYNC);
	if (fd < 0) { perror("open /dev/gpiomem"); return 1; }

	volatile uint32_t *pru = mmap(NULL, PRUSS_SIZE, PROT_READ | PROT_WRITE,
			MAP_SHARED, fd, PRUSS_BASE);
	if (pru == MAP_FAILED) { perror("mmap"); return 1; }

	volatile uint32_t *ctrl = &pru[CTRL_OFF / 4];
	volatile uint32_t *iram = &pru[IRAM_OFF / 4];
	volatile uint32_t *dram = &pru[0];
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
	printf("final PC=%08x SYSCFG=%08x\n", ctrl[1], *syscfg);
	return 0;
}
