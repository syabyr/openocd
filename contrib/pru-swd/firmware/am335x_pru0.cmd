/* AM335x PRU0 linker command for pru-swd
 *
 * Mailbox occupies PRU0 DRAM words 0..20 (byte 0x00-0x50), so .data/.bss
 * start above it at 0x100.  Code lives in IRAM at 0 (host loads the raw
 * binary image straight to IRAM; entry point is 0).
 */
-cr

MEMORY
{
	PAGE 0:  PRU_IMEM (RWX) : org = 0x00000000, len = 0x8000
	/* 0x100-0x400 only: .data/.bss/.stack must stay below the block
	 * read buffer, which the firmware and host place at DRAM 0x400.
	 * If the firmware ever outgrows this, the link fails loudly here
	 * instead of silently overlapping that buffer. */
	PAGE 1:  PRU_DMEM (RW)  : org = 0x00000100, len = 0x300
}

SECTIONS
{
	/* PRU always boots at PC=0: pin the CRT entry at absolute 0 */
	.text:_c_int00*	> 0x00000000
	.text	   :> PRU_IMEM, PAGE 0
	.rodata    :> PRU_IMEM, PAGE 0
	.data	   :> PRU_DMEM, PAGE 1
	.bss	   :> PRU_DMEM, PAGE 1
	.stack     :> PRU_DMEM, PAGE 1
}
