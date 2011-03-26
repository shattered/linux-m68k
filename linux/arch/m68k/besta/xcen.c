/*
 * besta/xcen.c -- Centronics write-only driver for HCPU30 board.
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
#include "hcpu30.h"

struct xcen {
	unsigned char   x0;
	unsigned char   x1;
	unsigned short  x2;
	void           *x4;
	unsigned char   x8;
	unsigned char   x9;
};

#define STATE_FREE      0
#define STATE_READY     1
#define STATE_WAIT      2
#define STATE_BREAK     3

static int xcen_busy = STATE_FREE;
static struct wait_queue *xcen_wait = NULL;

static int xcen_open (struct inode *inode, struct file *filp);
static void xcen_release (struct inode *inode, struct file *filp);
static int xcen_read (struct inode *inode, struct file *filp,
						char *buf, int count);
static int xcen_write (struct inode *inode, struct file *filp,
					    const char * buf, int count);
static void xcen_intr (int vec, void *data, struct pt_regs *fp);
static int xcen_ioctl (struct inode *inode, struct file *filp,
					unsigned int cmd, unsigned long arg);

static struct file_operations xcen_fops = {
	NULL,
	xcen_read,
	xcen_write,
	NULL,
	NULL,
	xcen_ioctl,
	NULL,
	xcen_open,
	xcen_release,
	NULL,
	NULL,
	NULL,
	NULL,
};

#define XCEN_MAJOR      48

static unsigned char xcen_buf[512];

void xcen_init (void) {
	volatile struct xcen *v = (struct xcen *) XCEN_ADDR;
	int vector, level;

	if (!besta_get_vect_lev ("xcen", &vector, &level)) {
		vector = get_unused_vector();
		level = XCEN_LEV;
	}

	v->x9 = vector;
	v->x8 = level;

	xcen_busy = STATE_FREE;

	if (register_chrdev (XCEN_MAJOR, "xcen", &xcen_fops)) {
	    printk ("Unable to get major %d\n", XCEN_MAJOR);
	    return;
	}

	besta_handlers[vector] = xcen_intr;
	besta_intr_data[vector] = NULL;

	return;
}

static int xcen_open (struct inode *inode, struct file *filp) {
	int minor = MINOR (inode->i_rdev);

	if (minor >= 2)  return -ENXIO;

	if (xcen_busy)  return -EBUSY;
	xcen_busy = STATE_READY;

	return 0;
}

static void xcen_release (struct inode *inode, struct file *filp) {

	xcen_busy = STATE_FREE;

	return;
}

static int xcen_read (struct inode *inode, struct file *filp,
						char *buf, int count) {
	return -ENOSYS;

}

static int xcen_write (struct inode *inode, struct file *filp,
					    const char * buf, int count) {
	volatile struct xcen *v = (struct xcen *) XCEN_ADDR;
	int minor = MINOR (inode->i_rdev);
	int err, blocks, n, i;
	unsigned short flags;

	err = verify_area (VERIFY_READ, buf, count);
	if (err)  return err;

	blocks = count / sizeof (xcen_buf);
	n = count % sizeof (xcen_buf);

	save_flags (flags);
	cli();

	while (blocks--) {
	    memcpy_fromfs (xcen_buf, (void *) buf, sizeof (xcen_buf));

	    if (minor == 0) {   /*  per bytes  */
		for (i = 0; i < sizeof (xcen_buf); i++) {
		    v->x2 = 1;
		    v->x4 = &xcen_buf[i];
		    v->x0 = 65;

		    xcen_busy = STATE_WAIT;
		    interruptible_sleep_on (&xcen_wait);
		    if (xcen_busy != STATE_READY) {
			xcen_busy = STATE_BREAK;
			restore_flags (flags);
			return -EINTR;
		    }
		}
	    } else {
		v->x2 = sizeof (xcen_buf);
		v->x4 = xcen_buf;
		v->x0 = 65;

		xcen_busy = STATE_WAIT;
		interruptible_sleep_on (&xcen_wait);
		if (xcen_busy != STATE_READY) {
		    xcen_busy = STATE_BREAK;
		    restore_flags (flags);
		    return -EINTR;
		}
	    }

	    buf += sizeof (xcen_buf);
	}

	if (n) {
	    memcpy_fromfs (xcen_buf, (void *) buf, n);

	    if (minor == 0) {   /*  per bytes  */
		for (i = 0; i < n; i++) {
		    v->x2 = 1;
		    v->x4 = &xcen_buf[i];
		    v->x0 = 65;

		    xcen_busy = STATE_WAIT;
		    interruptible_sleep_on (&xcen_wait);
		    if (xcen_busy != STATE_READY) {
			xcen_busy = STATE_BREAK;
			restore_flags (flags);
			return -EINTR;
		    }
		}
	    } else {
		v->x2 = n;
		v->x4 = xcen_buf;
		v->x0 = 65;

		xcen_busy = STATE_WAIT;
		interruptible_sleep_on (&xcen_wait);
		if (xcen_busy != STATE_READY) {
		    xcen_busy = STATE_BREAK;
		    restore_flags (flags);
		    return -EINTR;
		}
	    }
	}

	restore_flags (flags);

	return count;
}

static void xcen_intr (int vec, void *data, struct pt_regs *fp) {
	/*  It is very big routine.   */

	if (xcen_busy != STATE_WAIT)  return;
	wake_up_interruptible (&xcen_wait);
	xcen_busy = STATE_READY;

	return;
}

static int xcen_ioctl (struct inode *inode, struct file *filp,
					unsigned int cmd, unsigned long arg) {
	return -ENOSYS;

}
