// SPDX-License-Identifier: GPL-2.0
/*
 * gpiomem - restricted /dev/gpiomem for the OpenOCD am335xgpio and
 * pruswd drivers
 *
 * OpenOCD's am335xgpio adapter opens /dev/gpiomem (fallback /dev/mem) and
 * mmaps one page per GPIO bank, passing the bank's physical address as the
 * mmap() offset - i.e. /dev/mem semantics. The pruswd adapter additionally
 * mmaps the PRU-ICSS register window. This module provides exactly that,
 * but the mmap whitelist only admits the listed AM335x regions, so it is
 * not a full /dev/mem replacement.
 */
#include <linux/capability.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>

/* AM335x TRM ch.2 Memory Map.  Each entry is a whitelist region; the
 * mmap() offset must equal the region base and the requested length
 * must fit inside it (/dev/mem semantics, but restricted).
 */
static const struct {
	phys_addr_t base;
	unsigned long size;
} gpiomem_regions[] = {
	{ 0x44E07000, 0x1000 },	/* GPIO0 */
	{ 0x4804C000, 0x1000 },	/* GPIO1 */
	{ 0x481AC000, 0x1000 },	/* GPIO2 */
	{ 0x481AE000, 0x1000 },	/* GPIO3 */
	{ 0x4A300000, 0x40000 },	/* PRU-ICSS: PRU0 DRAM (mailbox), PRUx
					 * CTRL, IRAM — OpenOCD pru-swd driver
					 * loads its PRU firmware directly. */
};

static int gpiomem_open(struct inode *inode, struct file *file)
{
	if (!capable(CAP_SYS_RAWIO))
		return -EPERM;
	return 0;
}

static int gpiomem_mmap(struct file *file, struct vm_area_struct *vma)
{
	unsigned long size = vma->vm_end - vma->vm_start;
	phys_addr_t phys = (phys_addr_t)vma->vm_pgoff << PAGE_SHIFT;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(gpiomem_regions); i++) {
		if (phys == gpiomem_regions[i].base)
			break;
	}
	if (i == ARRAY_SIZE(gpiomem_regions)) {
		pr_err_ratelimited("gpiomem: mmap offset %pa not in whitelist\n",
				   &phys);
		return -EPERM;
	}

	if (size > gpiomem_regions[i].size)
		return -EINVAL;
	if (!(vma->vm_flags & VM_SHARED))
		return -EINVAL;

	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
	return io_remap_pfn_range(vma, vma->vm_start, vma->vm_pgoff, size,
				  vma->vm_page_prot);
}

static const struct file_operations gpiomem_fops = {
	.owner = THIS_MODULE,
	.open = gpiomem_open,
	.mmap = gpiomem_mmap,
};

static struct miscdevice gpiomem_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "gpiomem",
	.fops = &gpiomem_fops,
	.mode = 0600,
};

module_misc_device(gpiomem_misc);

MODULE_DESCRIPTION("AM335x GPIO + PRUSS register mmap for OpenOCD am335xgpio/pruswd");
MODULE_AUTHOR("mybays");
MODULE_LICENSE("GPL");
