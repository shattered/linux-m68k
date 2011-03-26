/* tiga.c
 *
 * Copyright (C) 1996 Jes Sorensen (jds@kom.auc.dk)
 *
 * Interface to TMS340x0 (TIGA) based graphic boards.
 */

#include <linux/types.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/major.h>
#include <linux/malloc.h>
#include <linux/mman.h>

#include <asm/setup.h>
#include <asm/page.h>
#include <asm/pgtable.h>
#include <asm/zorro.h>
#include <asm/amigahw.h>

#define TIGA_MAJOR	63
#define MAX_TIGA	2	/* Maximum number of Tiga boards */

#define TIGA_RESOLVER	1
#define TIGA_VIVID24	2
#define TIGA_A2410	3

struct TigaInfo {
	int in_use;
	void *iobase;		/* Base of the TMS340x0 I/O registers	*/
	unsigned long iosize;	/* Size of I/O-register area.		*/
	void *membase;		/*
				 * Base of board-memory, used by boards
				 * that share memory between the host and
				 * the GSP (like the Vivid24).
				 */
	unsigned long memsize;	/* Size of host/GSP shared memory.	*/
	int boardtype;
};

static struct TigaInfo tigainfo[MAX_TIGA];

static int
tiga_mmap(struct inode *inode, struct file *file, struct vm_area_struct * vma)
{
	unsigned int minor = MINOR(inode->i_rdev);

	if ((minor >= MAX_TIGA) || (tigainfo[minor].iobase == NULL))
		return -ENODEV;

	if (vma->vm_offset & ~PAGE_MASK)
		return -ENXIO;

	if ((vma->vm_end - vma->vm_start) > tigainfo[minor].iosize)
		return -EINVAL;

	if(m68k_is040or060){
		pgprot_val(vma->vm_page_prot) &= _CACHEMASK040;
		pgprot_val(vma->vm_page_prot) |= _PAGE_NOCACHE_S;
	}

	if(remap_page_range(vma->vm_start, ZTWO_VADDR(tigainfo[minor].iobase),
			    vma->vm_end - vma->vm_start, vma->vm_page_prot))
		return -EAGAIN;
	vma->vm_inode = inode;
	inode->i_count++;

	return 0;
}


static int tiga_open(struct inode *inode, struct file *file)
{
	unsigned int minor = MINOR(inode->i_rdev);

	if ((minor >= MAX_TIGA) || (tigainfo[minor].iobase == NULL))
		return -ENODEV;

	if (tigainfo[minor].in_use == 0){
		tigainfo[minor].in_use = 1;
		return 0;
	}
	else
		return -EBUSY;
}


static void tiga_release(struct inode *inode, struct file *file)
{
	unsigned int minor = MINOR(inode->i_rdev);

	tigainfo[minor].in_use = 0;
	return;
}


static struct file_operations tiga_fops = 
{
	NULL,           /* lseek        */
	NULL,           /* read         */
	NULL,           /* write        */
	NULL,           /* readdir      */
	NULL,           /* select       */
	NULL,           /* ioctl        */
	tiga_mmap,      /* mmap         */
	tiga_open,      /* open         */
	tiga_release,   /* release      */
	NULL            /* fsync        */
};


void gsp_init(void)
{
	int key, i;
	struct ConfigDev *cd;

	i = 0;

	/*
	 * Identify DMI Resolver boards.
	 */
	while ((i < MAX_TIGA) && (key = zorro_find(MANUF_APPLIED_MAGIC,
						   PROD_DMI_RESOLVER, 0, 0))){
		cd = zorro_get_board(key);
		tigainfo[i].iobase = cd->cd_BoardAddr;
		tigainfo[i].iosize = cd->cd_BoardSize;
		tigainfo[i].membase = NULL;
		tigainfo[i].memsize = 0;
		tigainfo[i].in_use = 0;
		tigainfo[i].boardtype = TIGA_RESOLVER;
		printk("Configuring DMI Resolver at 0x%08x as /dev/tiga%i.\n",
		       (unsigned) tigainfo[i].iobase, i);
		i++;
		zorro_config_board(key, 0);
	}
	/*
	 * Identify Commodore ULOWELL A2410 boards.
	 */
	while ((i < MAX_TIGA) &&
	       ((key = zorro_find(MANUF_UNIV_OF_LOWELL, PROD_A2410, 0, 0)) ||
		(key = zorro_find(MANUF_CARDCO, PROD_CC_A2410, 0, 0)))){

		cd = zorro_get_board(key);
		tigainfo[i].iobase = cd->cd_BoardAddr;
		tigainfo[i].iosize = cd->cd_BoardSize;
		tigainfo[i].membase = NULL;
		tigainfo[i].memsize = 0;
		tigainfo[i].in_use = 0;
		tigainfo[i].boardtype = TIGA_A2410;
		printk("Configuring A2410 at 0x%08x as /dev/tiga%i.\n",
		       (unsigned) tigainfo[i].iobase, i);
		i++;
		zorro_config_board(key, 0);
	}
	if (i > 0){
		if (register_chrdev(TIGA_MAJOR, "tiga", &tiga_fops)){
			printk("Unable to register TIGA device\n");
			return;
		}
	}
	/*
	 * Clear the tiga-structures that are not being used.
	 */
	for (; i < MAX_TIGA; i++)
		tigainfo[i].iobase = NULL;
}
