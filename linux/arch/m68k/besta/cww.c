/*
 * besta/cww.c -- Low-level serial driver for CWW8 (isio) VME-board.
 *		  Up to 8 serial ports.
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
#include <linux/major.h>

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
#include <asm/pgtable.h>
#include <asm/traps.h>
#include <asm/irq.h>

#include "besta.h"

struct odd {
	char          r0;
	unsigned char reg;
};

struct cww_board {
	union {
	    unsigned short status[2048];
	    struct {
		struct odd cntrl[4];
		struct odd vect[4];
	    } bim[256];
	} u;
	struct odd intr[1024];
	struct odd reset[1024];
	unsigned char taskbuf[12][1024];
	unsigned char rxbuf[8][1024];
	unsigned short strmem[2048];
	struct isio {
	    unsigned short icmd;
	    unsigned short ipar0;
	    unsigned long  ipar1;
	    unsigned long  ipar2;
	    unsigned short ipar3;
	    unsigned short ipar4;
	    unsigned short ocmd;
	    unsigned short opar0;
	    unsigned long  opar1;
	    unsigned long  opar2;
	    unsigned short opar3;
	    unsigned short opar4;
	} isio[8];
	unsigned short abort;
	unsigned short sysfail;
	unsigned short version;
	unsigned short echo;
	unsigned short parity;
	unsigned short disconn;
	unsigned short enverr;
	unsigned short x810e[889];
	struct isio_buf {
	    unsigned char ibuf[2048];
	    unsigned char obuf[2048];
	} isio_buf[8];
	unsigned short memory [31 * 1024];
};

static int cww_cnt = 0;

static struct cww_info {
	int     board_base;
	struct async_struct *serial_info_base;
	unsigned char serials;
	char          HSP_dev;
	unsigned char old_prom;
	unsigned char abort_busy;
	struct wait_queue *abort_wait;
} cww_info[MAX_SERIAL/8] = { { 0, }, };


static void cww_intr_0 (int vec, void *data, struct pt_regs *fp);
static void cww_intr_1 (int vec, void *data, struct pt_regs *fp);
static void cww_intr_2 (int vec, void *data, struct pt_regs *fp);
static void cww_intr_3 (int vec, void *data, struct pt_regs *fp);
static void cww_do_intr (int start, int end, void *data);


static void isio_tx_int (int vec, void *data, struct pt_regs *fp);
static void isio_init (struct async_struct *info);
static void isio_deinit (struct async_struct *info, int leave_dtr);
static void isio_enab_tx_int (struct async_struct *info, int enab_flag);
static int  isio_check_custom_divisor (struct async_struct *info,
				      int baud_base, int divisor);
static void isio_change_speed (struct async_struct *info);
static void isio_throttle (struct async_struct *info, int status);
static void isio_set_break (struct async_struct *info, int break_flag);
static void isio_get_serial_info (struct async_struct *info,
				struct serial_struct *retinfo);
static unsigned int isio_get_modem_info (struct async_struct *info);
static int isio_set_modem_info (struct async_struct *info, int new_dtr,
							      int new_rts);
static int isio_ioctl (struct tty_struct *tty, struct file *file,
		       struct async_struct *info, unsigned int cmd,
		       unsigned long arg);
static void isio_stop_receive (struct async_struct *info);
static int isio_trans_empty (struct async_struct *info);
static int isio_check_open (struct async_struct *info,
				 struct tty_struct *tty, struct file *file);

static SERIALSWITCH isio_switch = {
    isio_init, isio_deinit, isio_enab_tx_int,
    isio_check_custom_divisor, isio_change_speed,
    isio_throttle, isio_set_break,
    isio_get_serial_info, isio_get_modem_info,
    isio_set_modem_info, isio_ioctl, isio_stop_receive,
    isio_trans_empty, isio_check_open
};

static unsigned char isio_speed[15] = {
	0, 1, 2, 3, 4, 5, 6, 7, 9, 10, 11, 12, 13, 14, 15
};


/*  Returns 1 if we can assume the board has been resetted and clean.   */
static int cww_after_reset (volatile struct cww_board *cww) {
	int i;

	/*  there should be no sysfail reporting   */
	if (cww->sysfail)  return 0;

	/*  dog timer should be enabled   */
	if (!(cww->u.status[0] & 0x0400))  return 0;

	/*  BIM should be in initial state   */
	for (i = 0; i < 4; i++) {
	    if (cww->u.bim[0].cntrl[i].reg != 0)  return 0;
	    if (cww->u.bim[0].vect[i].reg != VEC_UNINT)  return 0;
	}

	/*  command ports should be in initial clean state   */
	for (i = 0; i < 8; i++) {
	    if (cww->isio[i].icmd != 0x8000)  return 0;
	    if (cww->isio[i].ocmd != 0x8000)  return 0;
	}

	/*  About tests above:
	    Be careful: theoretically there are no enough tests...
	*/
	return 1;
}


void cww_init (struct VME_board *VME, int on_off) {
	volatile struct cww_board *cww = (struct cww_board *) VME->addr;
	volatile unsigned char reset;
	int i, n;
	int vector[4];
	struct async_struct *info;
	int serial_type = 1000;

	if (on_off) {       /*  deinit   */

	    /*  reset the board into initial state. Most correct way...  */
	    if (VME_probe (&cww->reset[0].reg, 0, PROBE_READ, PORT_BYTE))
		    return;

	    /*  set bim into initial state   */
	    for (i = 0; i < 4; i++) {
		cww->u.bim[0].cntrl[i].reg = 0;
		cww->u.bim[0].vect[i].reg = VEC_UNINT;
	    }

	    return;
	}


	if (cww_cnt == MAX_SERIAL/8) {
	    printk ("    too many cww boards (0x%08x)\n", VME->addr);
	    return;
	}

	if (VME_probe (&cww->taskbuf[0][0], 0, PROBE_READ, PORT_BYTE)) {
	    printk ("    no %s at 0x%08x\n",
		    (VME->name ? VME->name : "board"), VME->addr);
	    return;
	}

	/*  get four vectors for bim...  */
	if (VME->vect < 0)
	    for (i = 0; i < 4; i++)  vector[i] = get_unused_vector();

	else {
	    if (VME->vect - VME->lev == VEC_SPUR) {
		printk ("Cannot register cww board at 0x%08x: "
			"don`t support autovectors\n", VME->addr);
		return;
	    }

	    for (i = 0; i < 4; i++)  vector[i] = VME->vect + i;
	    for (i = 0; i < 4; i++) {
		if (besta_handlers[vector[i]] != NULL) {
		    printk ("Cannot register cww board at 0x%08x: "
			    "some of vectors 0x%x 0x%x 0x%x 0x%x "
			    "already in use\n", VME->addr,
			    vector[0], vector[1], vector[2], vector[3]);
		    return;
		}
	    }
	}


	printk ("  0x%08x: ", VME->addr);

	if (!cww_after_reset (cww))  {
	    /*  There is not "after reset immediately" state,
	      so, it is needable to reset the board for autotest, etc.
		This stuff is needable because cww autotest continues
	      very long (about 4 seconds) at the boot time.
	    */

	    printk ("testing.");

	    /*  reset the cww board    */

	    reset = cww->reset[0].reg;

	    for (i = 0; i < 1000000; i++) ;     /*  paranoia   */
	    printk (".");

	    for (i = 0; i < 8; i++) {   /*  no longer than 8 seconds...  */
		int start = jiffies;

		while ((cww->u.status[0] & 0x0400) == 0 &&
		       jiffies < start + HZ
		) ;
		if (cww->u.status[0] & 0x0400)  break;

		printk (".");
	    }

	    if ((cww->u.status[0] & 0x0400) == 0) {
		printk ("FAILED! (ignore board)");

		if (cww->sysfail)
		    printk (" Hit L/R to turn off VME SYSFAIL!\n");
		else
		    printk ("\n");

		return;
	    }

	    printk ("OK: ");
	}

	VME->present = 1;       /*  OK, board is present   */

	cww_info[cww_cnt].board_base = VME->addr;
	cww_info[cww_cnt].old_prom = (cww->version & 0xf0) < 0x30;
	cww_info[cww_cnt].HSP_dev =
		(cww->version >> 8 != 0x3) ? (-1) : (cww->parity >> 1);
	cww_info[cww_cnt].serial_info_base = &rs_table[VME_serial_cnt];

	/*  notice   */
	printk ("CWW8");
	if (cww_info[cww_cnt].HSP_dev != -1)  printk ("-H");
	printk (", v=%02X, ser=0x%02x", cww->version >> 8,
						cww->version & 0xff);

#ifdef CONFIG_PROC_FS
	serial_type = besta_add_serial_type ("isio", 0);
	if (serial_type < 0)  serial_type = 1000;
#endif

	/*  Initialize the serial ports.   */
	for (n = 0; VME_serial_cnt + n < MAX_SERIAL && n < 8; n++) {

	    if (n == cww_info[cww_cnt].HSP_dev)  continue;

	    info = &rs_table[VME_serial_cnt + n];
	    info->type = serial_type;
	    info->board_base = (void *) cww_info[cww_cnt].board_base;
	    info->port = (int) &cww->isio[n];
	    info->irq = vector[n>>1];
	    info->IER = 0;      /*   c_cflag, c_iflag       */
	    info->MCR = 0;      /*  no carrier as default   */
	    info->sw = &isio_switch;
	}

	if (n) {
	    besta_handlers[vector[0]] = cww_intr_0;
	    besta_handlers[vector[1]] = cww_intr_1;
	    besta_handlers[vector[2]] = cww_intr_2;
	    besta_handlers[vector[3]] = cww_intr_3;

	    for (i = 0; i < 4; i++) {
		besta_intr_data[vector[i]] = &cww_info[cww_cnt];

		cww->u.bim[0].vect[i].reg = vector[i];
		cww->u.bim[0].cntrl[i].reg = VME->lev | 0x10;
	    }
	}

	cww_info[cww_cnt].serials = n;

	printk (", 8 serials ");
	if (n)  printk ("(%d,%d-%d) ", TTY_MAJOR,
				       SERIAL_START + VME_serial_cnt,
				       SERIAL_START + VME_serial_cnt + n - 1);
	else  printk ("(not used) ");
	if (cww_info[cww_cnt].HSP_dev != -1)  printk ("hsp (not used) ");
	printk ("\n");

	/*  release the board   */
	VME_serial_cnt += n;
	cww_cnt++;

	return;
}


#define DELAY_FOR_ABORT         30000       /*  assume it is enough...  */
#define DELAY_FOR_ASYNINI       10000000L   /*  should be enough...  */

static int do_abort (struct async_struct *info) {
	volatile struct cww_board *cww = (struct cww_board *) info->board_base;
	volatile struct isio *v = (struct isio *) info->port;
	int n, channel;
	volatile unsigned char intr;
	struct cww_info *cww_info = besta_intr_data[info->irq];

	if (!cww_info)  return -1;

	channel = v - &cww->isio[0];

retry_abort:
	while (cww_info->abort_busy) {
		interruptible_sleep_on (&cww_info->abort_wait);
		if (current->signal & ~current->blocked)  return -1;
	}
	cww_info->abort_busy = 1;

	if (!(v->ocmd & 0x8000)) {

	    cww->abort = 2 * channel + 2;
	    intr = cww->intr[0].reg;

	    n = DELAY_FOR_ABORT;
	    while (cww->abort && n--) ;
	    if (cww->abort)  goto cww_fail;
	}

	if (!(v->icmd & 0x8000)) {

	    cww->abort = 2 * channel + 1;
	    intr = cww->intr[0].reg;

	    n = DELAY_FOR_ABORT;
	    while (cww->abort && n--) ;
	    if (cww->abort)  goto cww_fail;
	}

	cww_info->abort_busy = 0;
	wake_up_interruptible (&cww_info->abort_wait);

	if (!(v->ocmd & 0x8000) || !(v->icmd & 0x8000)) {
	    current->state = TASK_INTERRUPTIBLE;
	    current->timeout = jiffies + HZ/20;

	    sti();
	    schedule();
	    cli();

	    if (!(v->ocmd & 0x8000) || !(v->icmd & 0x8000))
		    goto  retry_abort;
	}

	return 0;       /*  OK   */

cww_fail:
	intr = cww->reset[0].reg;

	cww_info->abort_busy = 0;
	wake_up_interruptible (&cww_info->abort_wait);

	info = cww_info->serial_info_base;
	for (n = 0; n < 8; n++)
	    if (info && info->tty)  set_bit (TTY_IO_ERROR, &info->tty->flags);

	return -1;
}

static void isio_change_speed (struct async_struct *info) {
	volatile struct cww_board *cww = (struct cww_board *) info->board_base;
	volatile struct isio *v = (struct isio *) info->port;
	volatile struct isio_buf *isio_buf =
		&cww->isio_buf[((struct isio *) info->port) - cww->isio];
	int n;
	unsigned short new_cflag, new_iflag, old_cflag, old_iflag;
	unsigned short flags;

	if (!info->tty || !info->tty->termios)  return;

	if (cww->sysfail) {
	    set_bit (TTY_IO_ERROR, &info->tty->flags);
	    return;
	}

	/*  because currently cannot access `old_termios' from here   */
	old_cflag = ((unsigned int) info->IER) >> 16;
	old_iflag = ((unsigned int) info->IER) & 0xffff;

	new_cflag = info->tty->termios->c_cflag;
	new_iflag = info->tty->termios->c_iflag;

	if (((old_cflag ^ new_cflag) &
	     (CBAUD | CSIZE | CLOCAL | CSTOPB | PARENB | PARODD)) == 0 &&
	    ((old_iflag ^ new_iflag) & (IXON | IXOFF | IXANY)) == 0
	)  return;      /*  the same hardware parameters   */


	save_flags (flags);
	cli();

	if ((!(v->ocmd & 0x8000) ||
	     !(v->icmd & 0x8000)) &&
	    do_abort (info)
	)  goto fail_change;    /*  abort failed   */

	/*  OK, both the channels are free...  */

	n = 0;
	if (!(new_cflag & CLOCAL))  n |= 0x1;
	if (new_iflag & IXON)  n |= 0x2;
	if (new_iflag & IXOFF)  n |= 0x4;
	if (new_iflag & IXANY)  n |= 0x8;

	isio_buf->obuf[0] = n;

	switch (new_cflag & CSIZE) {
	    case CS5:  isio_buf->obuf[1] = 0;  break;
	    case CS6:  isio_buf->obuf[1] = 1;  break;
	    case CS7:  isio_buf->obuf[1] = 2;  break;
	    case CS8:  isio_buf->obuf[1] = 3;  break;
	}

	if (new_cflag & CSTOPB)  isio_buf->obuf[2] = 0x0f;
	else if (isio_buf->obuf[1] == 0)  isio_buf->obuf[2] = 0;
	else  isio_buf->obuf[2] = 0x07;

	if (!(new_cflag & PARENB))  isio_buf->obuf[3] = 0;
	else if (new_cflag & PARODD)  isio_buf->obuf[3] = 0x6;
	else  isio_buf->obuf[3] = 0x2;

	isio_buf->obuf[4] =
		isio_buf->obuf[5] = isio_speed[(new_cflag & CBAUD) - 1];

	/*  delay, if old_prom ???   */

	v->ipar1 = (long) isio_buf->obuf - (long) info->board_base;
	v->ipar0 = 1;    /*  DTR ???   */

	v->icmd = 0x0006;       /*  ASYNINI_IN  */
	n = DELAY_FOR_ASYNINI;
	while (!(v->icmd & 0x8000) && n--) ;

	if (!(v->icmd & 0x8000))  goto fail_change;

#if 0
	/*  this cause old_prom isio to fail...  */

	v->opar1 = (long) isio_buf->obuf - (long) info->board_base;
	v->opar0 = 1;    /*  DTR ???   */

	v->ocmd = 0x0106;       /*  ASYNINI   */
	n = DELAY_FOR_ASYNINI;
	while (!(v->ocmd & 0x8000) && n--) ;

	if (!(v->ocmd & 0x8000))  goto fail_change;
#endif

	info->IER = (new_cflag << 16) | (new_iflag & 0xffff);

	if (new_cflag & CRTSCTS)
		info->flags |= ASYNC_CTS_FLOW;
	else
		info->flags &= ~ASYNC_CTS_FLOW;
	if (new_cflag & CLOCAL)
		info->flags &= ~ASYNC_CHECK_CD;
	else
		info->flags |= ASYNC_CHECK_CD;

	/*  start Rx   */
	v->ipar0 = 1;
	v->ipar2 = 64;
	v->icmd = 0x1015;       /*  GBWTO2   */

	isio_tx_int (0, info, 0);

	restore_flags (flags);

	return;

fail_change:
	set_bit (TTY_IO_ERROR, &info->tty->flags);

	restore_flags (flags);

	return;
}

static void isio_init (struct async_struct *info) {

	volatile struct isio *v = (struct isio *) info->port;

	/*  It is better to force `change_speed' hear, because
	  this time there are yet no active tasks in isio channels, i.e.,
	  nothing to abort. Normally `change_speed' is called immediately
	  after `init' method. For this second call all is already
	  initialized, so, nothing to change, no extra aborts...
	*/
	isio_change_speed (info);

	/*  Rx   */
	if ((v->icmd & 0x8000) == 0)
		v->icmd |= 0x1000;      /* will be started after intr */
	else {
	    /*  start Rx   */
	    v->ipar0 = 1;
	    v->ipar2 = 64;
	    v->icmd = 0x1015;       /*  GBWTO2   */
	}

	/*  Tx   */
	if ((v->ocmd & 0x8000) == 0)  v->ocmd |= 0x1000;

	return;
}

static void isio_deinit (struct async_struct *info, int leave_dtr) {
	volatile struct isio *v = (struct isio *) info->port;

	if (!(v->ocmd & 0x8000) ||
	    !(v->icmd & 0x8000)
	) {
	    if (do_abort (info))  return;     /*  abort failed   */
	    else  info->IER = 0;    /*  because aborted...  */
	}

	v->ocmd &= ~0x1000; /*  dis Tx  */
	v->icmd &= ~0x1000; /*  dis Rx  */

	return;
}

static void isio_enab_tx_int (struct async_struct *info, int enab_flag) {
	volatile struct isio *v = (struct isio *) info->port;
	unsigned short flags;

	if (enab_flag) {
	    save_flags (flags);
	    cli();

	    isio_tx_int (0, info, 0);

	    restore_flags (flags);
	} else
	    v->ocmd &= ~0x1000;     /*  dis Tx  */

	return;
}

static int isio_check_custom_divisor (struct async_struct *info,
					    int baud_base, int divisor) {
	return -1;

}

static void isio_throttle (struct async_struct *info, int status) {

	/*  nothing   */

	return;
}

static void isio_set_break (struct async_struct *info, int break_flag) {

	/*  nothing   */

	return;
}

static void isio_get_serial_info (struct async_struct *info,
					    struct serial_struct *retinfo) {

	retinfo->baud_base = info->baud_base;
	retinfo->custom_divisor = info->custom_divisor;

	return;
}

static unsigned int isio_get_modem_info (struct async_struct *info) {

	return (TIOCM_RTS | TIOCM_DTR | TIOCM_CAR | TIOCM_CTS | TIOCM_DSR);

}

static int isio_set_modem_info (struct async_struct *info,
						int new_dtr, int new_rts) {
	return -ENOSYS;

}

#define ISIO_GETADDR    (('I'<<8)|1)
static int isio_ioctl (struct tty_struct *tty, struct file *file,
		       struct async_struct *info, unsigned int cmd,
						   unsigned long arg) {
	int err;

	if (cmd == ISIO_GETADDR) {
	    err = verify_area (VERIFY_WRITE, (void *) arg, sizeof (int));
	    if (err)  return err;

	    memcpy_tofs ((void *) arg, &info->board_base, sizeof (int));
	} else
	    return -ENOIOCTLCMD;    /*  NOT `-EINVAL' !!!, because
					(Linux way) the line discipline
					receives this value and continues
					to check `cmd' value etc. */

	return 0;
}
#undef ISIO_GETADDR


static void isio_stop_receive (struct async_struct *info) {
	volatile struct isio *v = (struct isio *) info->port;

	v->icmd &= ~0x1000;

	return;
}

static int isio_trans_empty (struct async_struct *info) {
	volatile struct isio *v = (struct isio *) info->port;

	return (v->ocmd & 0x8000) != 0;
}

static int isio_check_open (struct async_struct *info,
				 struct tty_struct *tty, struct file *file) {
	volatile struct cww_board *cww = (struct cww_board *) info->board_base;
	int err, i;
	struct cww_info *cww_info = besta_intr_data[info->irq];

	if (file->f_flags == 0x1ca2) {

	    /*   It is a hackish svr3 opening:
	       if svr3 flags == 0xff then set ios area instead of
	       real opening.

		 Linux`s 0x1ca2 == svr3`s 0xfe
	    */

	    if (!cww_info)  return -ENXIO;

	    /*  We should be accuracy about some ports
	        are in ordinary using...
	    */

	    /*  mapping   */
	    err = VME_set_ios (cww_info->board_base,
				   cww_info->board_base,
					   sizeof (struct cww_board));
	    if (err)  return err;


	    /*  Problems hear:
	      We cannot return value `< 0', it cause open call to return `-1';
	      also we cannot return `> 0', it cause tty driver to work wrong.
	      So, we return `0', hoping (in this hackish implementation) user
	      will close file as soon as open it.
	    */

	    return 0;
	}

	/*  ordinary way.
	   Check for resetted state (reset takes about 4 seconds...)
	*/
	for (i = 0; (cww->u.status[0] & 0x0400) == 0 && i < 8; i++) {
	    current->state = TASK_INTERRUPTIBLE;
	    current->timeout = jiffies + HZ;

	    schedule();
	    if (current->signal & ~current->blocked)
		    return -ERESTARTSYS;
	}
	if ((cww->u.status[0] & 0x0400) == 0)  return -EIO;

	return 0;
}


static void isio_tx_int (int vec, void *data, struct pt_regs *fp) {
	struct async_struct *info = data;
	volatile struct cww_board *cww = (struct cww_board *) info->board_base;
	volatile struct isio *v = (struct isio *) info->port;
	volatile struct isio_buf *isio_buf =
		&cww->isio_buf[((struct isio *) info->port) - cww->isio];
	int bytes, tail;

	/*  Note: We don`t use `rs_get_tx_char' hear, because
	  it is possible to transmit more than one bytes at the time.
	    Be careful, if `rs_get_tx_char' is changed privately...
	*/

	if ((v->ocmd & 0x8000) == 0) {    /*  not ready   */
	    v->ocmd |= 0x1000;

	    return;
	}

	if (info->xmit_cnt <= 0 ||
	    info->tty->stopped ||
	    info->tty->hw_stopped
	) {
	    v->ocmd &= ~0x1000;     /*  dis Tx   */

	    return;
	}

	if (info->x_char) {
	    isio_buf->obuf[0] = info->x_char;
	    info->x_char = 0;

	    v->opar1 = (long) isio_buf->obuf - (long) info->board_base;
	    v->opar0 = 1;
	    v->ocmd = 0x1135;   /*  PUTCNT   */

	    return;
	}

	bytes = info->xmit_cnt > sizeof (isio_buf->obuf)
			? sizeof (isio_buf->obuf) : info->xmit_cnt;
	tail = SERIAL_XMIT_SIZE - info->xmit_tail;

	if (bytes <= tail) {
	    memcpy ((void *) isio_buf->obuf,
				&info->xmit_buf[info->xmit_tail], bytes);
	    info->xmit_tail += bytes;

	} else {
	    memcpy ((void *) isio_buf->obuf,
				&info->xmit_buf[info->xmit_tail], tail);
	    memcpy ((void *) &isio_buf->obuf[tail],
				&info->xmit_buf[0], bytes - tail);
	    info->xmit_tail = bytes - tail;
	}

	info->xmit_cnt -= bytes;
	info->xmit_tail &= (SERIAL_XMIT_SIZE - 1);

	v->opar1 = (long) isio_buf->obuf - (long) info->board_base;
	v->opar0 = bytes;
	v->ocmd = 0x1135;   /*  PUTCNT   */

	if (info->xmit_cnt < WAKEUP_CHARS)
		rs_sched_event (info, RS_EVENT_WRITE_WAKEUP);

	return;
}


/*  Various interrupt handler entryes.   */

static void cww_intr_0 (int vec, void *data, struct pt_regs *fp) {

	cww_do_intr (0, 2, data);

}

static void cww_intr_1 (int vec, void *data, struct pt_regs *fp) {

	cww_do_intr (2, 4, data);

}

static void cww_intr_2 (int vec, void *data, struct pt_regs *fp) {

	cww_do_intr (4, 6, data);

}

static void cww_intr_3 (int vec, void *data, struct pt_regs *fp) {

	cww_do_intr (6, 8, data);

}


static void cww_do_intr (int start, int end, void *data) {
	struct cww_info *cww_info = data;
	volatile struct cww_board *cww =
				(struct cww_board *) cww_info->board_base;
	volatile struct isio *v;
	struct async_struct *info;
	int i;

	for (i = start; i < end; i++) {
	    v = &cww->isio[i];      /*  may be uninitialized info`s  */
	    info = cww_info->serial_info_base + i;

	    if (i == cww_info->HSP_dev ||
		i >= cww_info->serials ||
		!(info->flags & ASYNC_INITIALIZED)
	    ) {
		v->icmd &= ~0x1000;
		v->ocmd &= ~0x1000;
		continue;
	    }


	    if ((v->icmd & 0x9000) == 0x9000) {
		volatile struct isio_buf *isio_buf = &cww->isio_buf[i];
		int stat, dcd, bytes, tail;
		volatile unsigned char *rbuf;

		v->icmd &= ~0x1000;

		/* Note: We don`t use `rs_receive_char' hear, because
		 it is possible to receive more than one bytes at the time.
		   Be careful, if `rs_receive_char' is changed privately...
		*/

		stat = v->icmd & 0x0f;
		dcd = (stat & 0x8) != 0;

		if (dcd != (info->MCR & 0x1)) {  /*  DCD  changed   */
		    if (dcd)  info->MCR |= 0x1;
		    else  info->MCR &= ~0x1;

		    rs_dcd_changed (info, dcd);
		}

		/*  receiving   */

		rbuf = (unsigned char *)
			    ((long) info->board_base + v->ipar1);

		bytes = TTY_FLIPBUF_SIZE - info->tty->flip.count;
		bytes = bytes <= v->ipar0 ? bytes : v->ipar0;

		tail = (isio_buf->ibuf - rbuf) + sizeof (isio_buf->ibuf);

		if (bytes <= tail) {
		    memcpy (info->tty->flip.char_buf_ptr,
					    (void *) rbuf, bytes);
		} else {
		    memcpy (info->tty->flip.char_buf_ptr,
					    (void *) rbuf, tail);
		    memcpy (&info->tty->flip.char_buf_ptr[tail],
				(void *) isio_buf->ibuf, bytes - tail);
		}
		memset (info->tty->flip.flag_buf_ptr, 0, bytes);

		info->tty->flip.count += bytes;
		info->tty->flip.char_buf_ptr += bytes;
		info->tty->flip.flag_buf_ptr += bytes;

		/*  Checking for Rx errors   */
		if (!cww_info->old_prom && (stat & 0x7)) {

		    if (stat & 0x01)
			info->tty->flip.flag_buf_ptr[-1] = TTY_PARITY;
		    else if (stat & 0x4) {
			info->tty->flip.flag_buf_ptr[-1] = TTY_BREAK;
			if (info->flags & ASYNC_SAK)  do_SAK(info->tty);
		    } else
			info->tty->flip.flag_buf_ptr[-1] = TTY_FRAME;
		}

		queue_task_irq(&info->tty->flip.tqueue, &tq_timer);

		v->ipar0 = 1;
		v->ipar2 = 64;
		v->icmd = 0x1015;       /*  GBWTO2   */
	    }

	    if ((v->ocmd & 0x9000) == 0x9000) {
		v->ocmd &= ~0x1000;

		isio_tx_int (0, info, 0);
	    }
	}

	return;
}

