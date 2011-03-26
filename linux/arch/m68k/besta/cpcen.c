/*
 * besta/cpcen.c -- Centronics driver (write-only) for CP31.
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
#include "cp31.h"

#define STATE_FREE      0
#define STATE_READY     1
#define STATE_WAIT      2
#define STATE_BREAK     3

static int cen_busy = STATE_FREE;
static struct wait_queue *cen_wait = NULL;

static int cen_open (struct inode *inode, struct file *filp);
static void cen_release (struct inode *inode, struct file *filp);
static int cen_read (struct inode *inode, struct file *filp,
						char *buf, int count);
static int cen_write (struct inode *inode, struct file *filp,
					    const char * buf, int count);
static void cen_intr (int vec, void *data, struct pt_regs *fp);
static int cen_ioctl (struct inode *inode, struct file *filp,
					unsigned int cmd, unsigned long arg);

static struct file_operations cen_fops = {
	NULL,
	cen_read,
	cen_write,
	NULL,
	NULL,
	cen_ioctl,
	NULL,
	cen_open,
	cen_release,
	NULL,
	NULL,
	NULL,
	NULL,
};

#define CEN_MAJOR      48

int cp31_ceninit (void) {
	volatile struct pit *pit = (struct pit *) CEN_ADDR;
	volatile struct bim *bim = (struct bim *) BIM_ADDR;
	int vector, level;
	unsigned char old, new;
	unsigned int value;

	/*   pit->b_dir may store data, and we can assume there are no
	   any side effects, because pit port B is not used.
	     So, we try to write, read and compare. If OK, assume
	   it is mc68230 pit, else it is not present or empty area.
	*/

	if (VME_probe (&pit->b_dir, &value, PROBE_READ, PORT_BYTE))  return 0;

	old = value;
	new = old ^ 0x01;
	value = new;
	if (VME_probe (&pit->b_dir, &value, PROBE_WRITE, PORT_BYTE) ||
	    pit->b_dir != new       /*  i.e., can not store   */
	)  return 0;

	pit->b_dir = old;   /*  restore `right' value   */


	cen_busy = STATE_FREE;

	if (register_chrdev (CEN_MAJOR, "cen", &cen_fops)) {
	    printk ("Unable to get major %d\n", CEN_MAJOR);
	    return 1;
	}

	/*  initialize pit mc68230 port   */
	pit->gen_cntl = 0x10;
	pit->serv_req = 0x0a;
	pit->a_dir = 0xff;
	pit->b_dir = 0x00;
	pit->c_dir = 0x00;
	pit->a_cntl |= 0xb0;
	pit->status = 0x01;

	if (!besta_get_vect_lev ("cen", &vector, &level)) {
		vector = get_unused_vector();
		level = CEN_LEV;
	}

	bim->vect[3].reg = vector;
	bim->cntrl[3].reg = level | 0x10;

	besta_handlers[vector] = cen_intr;
	besta_intr_data[vector] = NULL;

	return 1;
}

static int cen_open (struct inode *inode, struct file *filp) {
	int minor = MINOR (inode->i_rdev);

	if (minor > 0)  return -ENXIO;

	if (cen_busy)  return -EBUSY;
	cen_busy = STATE_READY;

	return 0;
}

static void cen_release (struct inode *inode, struct file *filp) {

	cen_busy = STATE_FREE;

	return;
}

static int cen_read (struct inode *inode, struct file *filp,
						char *buf, int count) {
	return -ENOSYS;

}

static int cen_write (struct inode *inode, struct file *filp,
					    const char * buf, int count) {
	volatile struct pit *pit = (struct pit *) CEN_ADDR;
	int n = count;
	unsigned short flags;

	save_flags (flags);
	cli();

	while (n > 0) {
	    unsigned char ch;

	    ch = get_user (buf);

#if 0
	    /*  it is needable hear ???   */
	    pit->gen_cntl &= ~0x10;
	    pit->gen_cntl |= 0x10;
#endif

	    pit->a_data = ch;
	    pit->a_cntl = 0xba;
	    pit->a_cntl = 0xb2;

	    cen_busy = STATE_WAIT;
	    interruptible_sleep_on (&cen_wait);
	    if (cen_busy != STATE_READY) {
		cen_busy = STATE_BREAK;
		count = -EINTR;
		break;
	    }

	    n--;
	    buf++;
	}

	restore_flags (flags);

	return count;
}

static void cen_intr (int vec, void *data, struct pt_regs *fp) {
	volatile struct pit *pit = (struct pit *) CEN_ADDR;

	((volatile struct pit *) PIT_ADDR)->status = 0x08;

	pit->a_cntl &= ~0x02;
	pit->gen_cntl &= ~0x10;
	pit->gen_cntl |= 0x10;

	if (cen_busy != STATE_WAIT)  return;
	wake_up_interruptible (&cen_wait);
	cen_busy = STATE_READY;

	return;
}

static int cen_ioctl (struct inode *inode, struct file *filp,
					unsigned int cmd, unsigned long arg) {
	return -ENOSYS;

}
