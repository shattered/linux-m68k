/*
 * besta/md10.c -- Driver for MD10 (MD1) VME-board. A parallel interface
 *		   with ioctl(2)-style access.
 *
 * Copyright 1996, 1997	    Dmitry K. Butskoy
 *			    <buc@citadel.stu.neva.ru>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2, or (at your option) any
 * later version.
 *
 */

#include <linux/config.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/kd.h>
#include <linux/sched.h>
#include <linux/errno.h>
#include <linux/tty.h>
#include <linux/console.h>
#include <linux/termios.h>
#include <linux/interrupt.h>
#include <linux/serial.h>
#include <linux/linkage.h>
#include <linux/kernel_stat.h>
#include <linux/major.h>

#include <asm/system.h>
#include <asm/pgtable.h>
#include <asm/traps.h>
#include <asm/irq.h>

#include "besta.h"

#define MD10_MAX_BOARDS 16

static int md10_cnt = 0;
static int md10_registered = 0;

unsigned short *md10_addrs[MD10_MAX_BOARDS] = { NULL, };

static int md10_open (struct inode *inode, struct file *filp);
static int md10_ioctl (struct inode *inode, struct file *filp,
					unsigned int cmd, unsigned long arg);

static struct file_operations md10_fops = {
	NULL,
	NULL,       /*  no read   */
	NULL,       /*  no write  */
	NULL,
	NULL,
	md10_ioctl,
	NULL,
	md10_open,
	NULL,       /*  no release  */
	NULL,
	NULL,
	NULL,
	NULL,
};


void md10_init (struct VME_board *VME, int on_off) {
	volatile unsigned short *md10 = (unsigned short *) VME->addr;

	if (on_off)  return;    /*  nothing to deinit   */

	if (VME_probe (md10, 0, PROBE_READ, PORT_WORD)) {
	    printk ("    no %s at 0x%08x\n",
		    (VME->name ? VME->name : "board"), VME->addr);
	    return;
	}

	VME->present = 1;       /*  OK, board is present   */

	if (!md10_registered) {
	    if (register_chrdev (VME->major, VME->name, &md10_fops)) {
		printk ("Unable to get major %d\n", VME->major);
		return;
	    }
	    md10_registered = 1;
	}

	printk ("  0x%08x: MD10 board ", VME->addr);

	if (md10_cnt < MD10_MAX_BOARDS) {

	    printk ("(%d,%d) \n", VME->major, md10_cnt);

	    md10_addrs[md10_cnt] = (unsigned short *) VME->addr;
	    md10_cnt++;
	} else
	    printk ("(not used -- Too many (%d) boards)\n", md10_cnt);

	return;
}

static int md10_open (struct inode *inode, struct file *filp) {
	int minor = MINOR (inode->i_rdev);

	if (minor >= md10_cnt)  return -ENXIO;

	return 0;
}

static int md10_ioctl (struct inode *inode, struct file *filp,
					unsigned int cmd, unsigned long arg) {
	int minor = MINOR (inode->i_rdev);
	int err;
	volatile unsigned short *md10 = (unsigned short *) md10_addrs[minor];
	unsigned short tmp;

	switch (cmd) {
	    case 0:     /*  read   */
		err = verify_area (VERIFY_WRITE, (void *) arg,
						    sizeof (unsigned short));
		if (err)  return err;

		tmp = *md10;
		memcpy_tofs ((void *) arg, &tmp, sizeof (unsigned short));

		break;

	    case 1:     /*  write   */
		err = verify_area (VERIFY_READ, (void *) arg,
						    sizeof (unsigned short));
		if (err)  return err;

		memcpy_fromfs (&tmp, (void *) arg, sizeof (unsigned short));
		*md10 = tmp;

		break;

	    case 2:     /*  some checks ???   */
		return -ENOSYS;
		break;

	    default:
		return -EINVAL;
		break;
	}

	return 0;
}
