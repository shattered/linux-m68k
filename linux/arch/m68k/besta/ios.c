/*
 * besta/ios.c -- Sysv-style `/dev/ios' driver.
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
#include <linux/mman.h>
#include <linux/malloc.h>
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


static int ios_open (struct inode *inode, struct file *filp);
static void ios_release (struct inode *inode, struct file *filp);
static int ios_ioctl (struct inode *inode, struct file *filp,
					unsigned int cmd, unsigned long arg);

static struct file_operations ios_fops = {
	NULL,
	NULL,       /*  no read   */
	NULL,       /*  no write  */
	NULL,
	NULL,
	ios_ioctl,
	NULL,
	ios_open,
	ios_release,
	NULL,
	NULL,
	NULL,
	NULL,
};

#define IOS_BUF_SIZE    8

struct ios_info {
	struct task_struct *task;
	unsigned int     vaddr;
	unsigned int     paddr;
	unsigned int     size;
	unsigned int     vset;
	unsigned int     pset;
	unsigned int     sizeset;
} ios_buf[IOS_BUF_SIZE] = {{0, }, };


void ios_init (const char *name, int major) {

	if (register_chrdev (major, name, &ios_fops))
	    printk ("Unable to get major %d\n", major);

	return;
}


static int ios_open (struct inode *inode, struct file *filp) {
	int i;
	unsigned short flags;

	for (i = 0; i < IOS_BUF_SIZE; i++)
	    if (ios_buf[i].task == current)  break;
	if (i == IOS_BUF_SIZE) {
	    save_flags (flags);
	    cli();

	    for (i = 0; i < IOS_BUF_SIZE; i++)
		if (ios_buf[i].task == NULL) {
		    ios_buf[i].task = current;
		    ios_buf[i].vset = 0;
		    ios_buf[i].pset = 0;
		    ios_buf[i].sizeset = 0;
		    break;
		}

	    restore_flags (flags);

	    if (i == IOS_BUF_SIZE)  return -EBUSY;
	}

	return 0;
}

static void ios_release (struct inode *inode, struct file *filp) {
	int i;
	for (i = 0; i < IOS_BUF_SIZE; i++)
	    if (ios_buf[i].task == current)  ios_buf[i].task = NULL;

	return;
}

static int ios_ioctl (struct inode *inode, struct file *filp,
					unsigned int cmd, unsigned long arg) {
	int i;

	for (i = 0; i < IOS_BUF_SIZE; i++)
	    if (ios_buf[i].task == current)  break;
	if (i == IOS_BUF_SIZE)  return -EBUSY;

	switch (cmd) {
	    case 0:
		ios_buf[i].vaddr = arg;
		ios_buf[i].vset = 1;
		break;
	    case 1:
		ios_buf[i].paddr = arg;
		ios_buf[i].pset = 1;
		break;
	    case 2:
		ios_buf[i].size = arg;
		ios_buf[i].sizeset = 1;
		break;
	    default:
		return -EINVAL;
	}

	if (!ios_buf[i].vset || !ios_buf[i].pset || !ios_buf[i].sizeset)
								return 0;
	return  VME_set_ios (ios_buf[i].vaddr, ios_buf[i].paddr,
							ios_buf[i].size);
}

