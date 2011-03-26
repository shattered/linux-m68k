/*
 * besta/xdus.c -- Low level serial driver for HCPU30`s console and
 *		   second port. Without modem stuff - no info for this.
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
#include <linux/linkage.h>
#include <linux/kernel_stat.h>

/*  Change `queue_task_irq' to interrupt-independent `queue_task',
  because different serial ports may have different interrupt levels (m68k!),
  so, any changes in serial interrupt handlers are not atomic
  to their common data...
    Assume `queue_task_irq' appears only in <linux/serial.h> inline`s ...
*/
#define queue_task_irq(A,B)     queue_task (A, B)
#include <linux/serial.h>
#undef queue_task_irq

#include <asm/system.h>
#include <asm/machdep.h>
#include <asm/pgtable.h>
#include <asm/traps.h>
#include <asm/irq.h>

#include "besta.h"
#include "hcpu30.h"

static void hcpu30_debug_intr (int vec, void *data, struct pt_regs *fp);

struct xdus {
	char x0;
	char x1;
	char r2; char r3; char r4; char r5; char r6; char r7;
	char x8;
	char r9; char ra; char rb; char rc; char rd; char re; char rf;
	char x10;
	char x11;
	char x12;
	char r13;
	unsigned short x14;
	unsigned short x16;
};

/*   Currently I don`t know all about xdus, bitmasks gets from
   identical sysv dumb driver...
     Should we use 0x1000 bit in iflag_mask ???
*/
#define XDUS_CMASK      0x0b7f
#define XDUS_IMASK      0x0400

static void xdus_rx_int (int vec, void *data, struct pt_regs *fp);
static void xdus_tx_int (int vec, void *data, struct pt_regs *fp);
static void xdus_init (struct async_struct *info);
static void xdus_deinit (struct async_struct *info, int leave_dtr);
static void xdus_enab_tx_int (struct async_struct *info, int enab_flag);
static int  xdus_check_custom_divisor (struct async_struct *info,
				      int baud_base, int divisor);
static void xdus_change_speed (struct async_struct *info);
static void xdus_throttle (struct async_struct *info, int status);
static void xdus_set_break (struct async_struct *info, int break_flag);
static void xdus_get_serial_info (struct async_struct *info,
				struct serial_struct *retinfo);
static unsigned int xdus_get_modem_info (struct async_struct *info);
static int xdus_set_modem_info (struct async_struct *info, int new_dtr,
			      int new_rts);
static int xdus_ioctl (struct tty_struct *tty, struct file *file,
		       struct async_struct *info, unsigned int cmd,
		       unsigned long arg);
static void xdus_stop_receive (struct async_struct *info);
static int xdus_trans_empty (struct async_struct *info);


/*  SERIALSWITCH structure for the HCPU30 xduss ports.  */

static SERIALSWITCH xdus_switch = {
    xdus_init, xdus_deinit, xdus_enab_tx_int,
    xdus_check_custom_divisor, xdus_change_speed,
    xdus_throttle, xdus_set_break,
    xdus_get_serial_info, xdus_get_modem_info,
    xdus_set_modem_info, xdus_ioctl,
    xdus_stop_receive, xdus_trans_empty, NULL
};


/*         Initialize routine.                  */

int hcpu30_xdusinit (struct async_struct *info, int rs_count) {
    volatile struct xdus *v;
    int serial_type = 1000;
    int vector, level;

#ifdef CONFIG_PROC_FS
    serial_type = besta_add_serial_type ("xdus (hcpu30)", 0);
    if (serial_type < 0)  serial_type = 1000;
#endif

/*      Initialize first xdus (console).    */
    info = &info[rs_count++];

    if (!besta_get_vect_lev ("xdus", &vector, &level)) {
	vector = get_unused_vector();
	level = XDUS_LEV;
    }

    info->type = serial_type;
    info->port = XDUS_ADDR;
    info->irq = vector;
    info->sw = &xdus_switch;

    besta_handlers[vector] = xdus_rx_int;
    besta_intr_data[vector] = info;

    v = (struct xdus *) XDUS_ADDR;
    v->x12 = vector;
    v->x11 = level;
    v->x0 = 66;


    if (!besta_get_vect_lev ("xdus1", &vector, &level)) {
	vector = get_unused_vector();
	level = XDUS1_LEV;
    }

    if (!hcpu30_debug_mode) {
	/*      Initialize second xdus     */

	info = &info[rs_count++];
	info->type = serial_type;
	info->port = XDUS1_ADDR;
	info->irq = vector;
	info->sw = &xdus_switch;

	besta_handlers[vector] = xdus_rx_int;
	besta_intr_data[vector] = info;

	v = (struct xdus *) XDUS1_ADDR;
	v->x12 = vector;
	v->x11 = level;
	v->x0 = 66;

    } else {

	besta_handlers[vector] = hcpu30_debug_intr;
	besta_intr_data[vector] = NULL;

	v = (struct xdus *) XDUS1_ADDR;
	v->x12 = vector;
	v->x11 = 7;     /*  non-masked interrupt level   */
	v->x0 = 66;
    }

    return rs_count;
}

static void xdus_init (struct async_struct *info) {
	volatile struct xdus *v = (struct xdus *) info->port;

	/*   For the console port get initial termios flags by hardware.
	   This way we correctly get console port speed etc. (Very actual when
	   somewhat runs without getty(1M) -- i.e. `init 1' level).
	*/
	if(info->port == XDUS_ADDR && info->tty && info->tty->termios) {
	    struct termios *termios = info->tty->termios;

	    termios->c_cflag =
		    (termios->c_cflag & ~XDUS_CMASK) | (v->x14 & XDUS_CMASK);
	    termios->c_iflag =
		    (termios->c_iflag & ~XDUS_IMASK) | (v->x16 & XDUS_IMASK);
	}
	v->x0 = 66;

	return;
}


static void xdus_deinit (struct async_struct *info, int leave_dtr) {

	return;
}

static void xdus_enab_tx_int (struct async_struct *info, int enab_flag) {
	unsigned short flags;

	if (enab_flag) {
	    save_flags (flags);
	    cli();

	    xdus_tx_int (0, info, 0);

	    restore_flags (flags);
	}

	return;
}

static int  xdus_check_custom_divisor (struct async_struct *info,
				      int baud_base, int divisor) {
	return -1;  /* cannot support custom divisor ???  */
}

static void xdus_change_speed (struct async_struct *info) {
	volatile struct xdus *v = (struct xdus *) info->port;

	unsigned short old_cflag, old_iflag, new_cflag, new_iflag;

	if(!info->tty || !info->tty->termios) return;

	new_cflag = info->tty->termios->c_cflag;
	new_iflag = info->tty->termios->c_iflag;

	old_cflag = v->x14;
	old_iflag = v->x16;
	v->x14 = new_cflag;
	v->x16 = new_iflag;

	if (new_cflag & CRTSCTS)
		info->flags |= ASYNC_CTS_FLOW;
	else
		info->flags &= ~ASYNC_CTS_FLOW;
	if (new_cflag & CLOCAL)
		info->flags &= ~ASYNC_CHECK_CD;
	else
		info->flags |= ASYNC_CHECK_CD;

	if(((old_cflag ^ new_cflag) & XDUS_CMASK) |
	   ((old_iflag ^ new_iflag) & XDUS_IMASK)
	) {
		unsigned long flags;

		save_flags (flags);
		cli();

		v->x0 = 1;
		while(v->x0 > 0) ;
		if(info->flags & ASYNC_INITIALIZED) v->x0 = 66;

		restore_flags (flags);
	}
	return;
}

static void xdus_throttle (struct async_struct *info, int status) {
	/* nothing  */
}

static void xdus_set_break (struct async_struct *info, int break_flag) {
	/* nothing  */
}

static void xdus_get_serial_info (struct async_struct *info,
				struct serial_struct *retinfo) {

	retinfo->baud_base = info->baud_base;
	retinfo->custom_divisor = info->custom_divisor;
}

static unsigned int xdus_get_modem_info (struct async_struct *info) {

	return ( TIOCM_RTS | TIOCM_DTR | TIOCM_CAR | TIOCM_CTS | TIOCM_DSR );
}

static int xdus_set_modem_info (struct async_struct *info, int new_dtr,
			      int new_rts) {
	/* nothing  */

	return -EINVAL;
}

static void xdus_stop_receive (struct async_struct *info) {
	volatile struct xdus *v = (struct xdus *) info->port;

	v->x0 = 1;

	return;
}

static int xdus_trans_empty (struct async_struct *info) {
	volatile struct xdus *v = (struct xdus *) info->port;

	return (v->x1 <= 0) ;
}

static void xdus_tx_int (int vec, void *data, struct pt_regs *fp) {
	struct async_struct *info = data;
	volatile struct xdus *v = (struct xdus *) info->port;

	if (v->x1 <= 0) {
	    int ch;

	    if ((ch = rs_get_tx_char (info)) >= 0) {
		v->x10 = ch;
		v->x1 = 66;
	    } else
		return;

	    /*  no needable to dis tx int hear   */
	}
	return;
}

static void xdus_rx_int (int vec, void *data, struct pt_regs *fp) {
	struct async_struct *info = data;
	volatile struct xdus *v = (struct xdus *) info->port;
	int ch;

	if(!(info->flags & ASYNC_INITIALIZED)) return;

	if(v->x0 <= 0) {

		ch = v->x8;
		v->x0 = 66;
		rs_receive_char (info, ch, 0);
	}

	xdus_tx_int (vec, info, fp);
	return;
}


/* Routine for kernel messages onto console  */
void xdus_putstring (const char *s) {
	volatile struct xdus *v = (struct xdus *) XDUS_ADDR;
	unsigned long flags;
	char c;

	save_flags (flags);
	cli();

	while((c = *s++) != 0) {
		while(v->x1 > 0) ;
		if (c == '\n') {
			v->x10 = '\r';
			v->x1 = 2;
			while(v->x1 > 0) ;
		}
		v->x10 = c;
		v->x1 = 2;
	}

	while(v->x1 > 0) ;

	restore_flags (flags);

	return;
}


/*      KERNEL DEBUG STUFF          */

static void show_all (struct pt_regs *fp);
static void xdus1_putstring (const char *s);
static unsigned char xdus1_getchar (void);
static void sprint (const char *fmt, ...);

/*  Routine to handler special level 7 interrupt from any serial
  to debug system work.
*/
void hcpu30_debug_intr (int vec, void *data, struct pt_regs *fp) {
	volatile struct xdus *v = (struct xdus *) XDUS1_ADDR;
	char c;

	if (v->x0 > 0)  return;
	c = v->x8 & 0x7f;

	if (c != 003) {     /*  should be '^C' to level 7 interrupt   */
	    v->x0 = 66;
	    return;
	}
repeat:
	xdus1_putstring ("\nKERNEL DEBUG:\n");
	show_all (fp);
	xdus1_putstring ("c - continue, r - repeat \n");
getchar:
	c = xdus1_getchar();
	if (c == 'c') {
	    v->x0 = 66;
	    return;
	} else if (c == 'r')  goto repeat;
	else goto getchar;

	return;
}

static void show_all (struct pt_regs *fp) {
	unsigned long sp;

	sprint("PC:    %08lx      SR: %04x\n", fp->pc,fp->sr);

	__asm ("mov.l %%usp,%0" : "=d" (sp) : );
	sprint ("usp: %08lx    ", sp);
	__asm ("mov.l %%msp,%0" : "=d" (sp) : );
	sprint ("msp: %08lx    ", sp);
	__asm ("mov.l %%isp,%0" : "=d" (sp) : );
	sprint ("isp: %08lx    ", sp);
	sprint ("\n");

	if (STACK_MAGIC != *(unsigned long *)current->kernel_stack_page)
		sprint("Corrupted stack page\n");
	sprint("Process %s (pid: %d, stackpage=%08lx)\n",
		current->comm, current->pid, current->kernel_stack_page);

	return;
}


/*  Routine for debug messages onto second serial port.  */
static void xdus1_putstring (const char *s) {
	volatile struct xdus *v = (struct xdus *) XDUS1_ADDR;
	char c;

	while((c = *s++) != 0) {
		while(v->x1 > 0) ;
		if (c == '\n') {
			v->x10 = '\r';
			v->x1 = 2;
			while(v->x1 > 0) ;
		}
		v->x10 = c;
		v->x1 = 2;
	}

	while(v->x1 > 0) ;

	return;
}

static unsigned char xdus1_getchar (void) {
	volatile struct xdus *v = (struct xdus *) XDUS1_ADDR;
	char c;

	while(v->x0 > 0) ;
	v->x0 = 2;
	while(v->x0 > 0) ;
	c = v->x8 & 0x7f;

	return c;
}

static char buf[1024];

static void sprint (const char *fmt, ...) {
	int i;
	va_list args;

	va_start(args, fmt);
	i = vsprintf(buf, fmt, args);
	va_end(args);

	buf[i] = 0;
	xdus1_putstring (buf);

	return;
}


/*  Someone use /dev/console to access hardware clock. Be-e-e-e-e!!!
   Sorry. They developed the design without me...
*/
static int xdus_ioctl (struct tty_struct *tty, struct file *file,
		       struct async_struct *info, unsigned int cmd,
						    unsigned long arg) {
	int err;
	struct hwclk_time time;

	/*  only console dev allow it   */
	if (info->port != XDUS_ADDR)  return -ENOIOCTLCMD;

	/* Linux/68k interface to the hardware clock */
	switch (cmd) {
	    case KDGHWCLK:

		err = verify_area (VERIFY_WRITE, (void *) arg, sizeof (time));
		if (err)  return err;

		if (!mach_hwclk)  return -ENOSYS;

		err = mach_hwclk (0, &time);
		if (err)  return err;

		memcpy_tofs ((void *) arg, &time, sizeof (time));

		return 0;
		break;

	    case KDSHWCLK:

		if (!suser())  return -EPERM;

		err = verify_area (VERIFY_READ, (void *) arg, sizeof (time));

		if (err)  return err;

		memcpy_fromfs (&time, (void *) arg, sizeof (time));

		/*  checking...   */
		if (!mach_hwclk)  return -ENOSYS;
		if (time.sec  > 59 ||
		    time.min  > 59 ||
		    time.hour > 23 ||
		    time.day  < 1  || time.day  > 31 ||
		    time.mon  > 11 ||
		    time.wday < -1 || time.wday > 6  ||
		    time.year < 70
		)  return -EINVAL;

		return  mach_hwclk (1, &time);

	    default:
		return -ENOIOCTLCMD;
		break;
	}

	return 0;       /*  not reached   */
}

