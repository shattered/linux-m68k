/*
 * besta/cpsio.c -- Serial driver for CP20/CP30/CP31 console
 *		    (and may be additional main-board serials - not tested).
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
#include "cp31.h"

struct sio {
	unsigned char   r_stat;
	unsigned char   r_cntl;
	unsigned char   r_data;
	char            r3;
	unsigned char   r_vect;
	unsigned char   r_ena;
	char            r6;
	char            r7;
	unsigned char   t_stat;
	unsigned char   t_cntl;
	unsigned char   t_data;
	char            rb;
	unsigned char   t_vect;
	unsigned char   t_ena;
	char            re;
	char            rf;
	unsigned char   s_stat;
	unsigned char   s_cntl;
	unsigned char   r12;    /*  probe port for testing `sio or pit?'...  */
	char            r13;
	unsigned char   s_vect;
	unsigned char   s_ena;
	char            r16;
	char            r17;
	unsigned char   prot_sel0;
	unsigned char   prot_sel1;
	unsigned char   ar0;
	unsigned char   ar1;
	unsigned char   baud_div0;
	unsigned char   baud_div1;
	unsigned char   clock_cntl;
	unsigned char   err_cntl;
};

unsigned char sio_magics[3][15] = {
  /* 50   75  110  135  150  200  300  600 1200 1800 2400 4800 9600  19k  38k*/
  {0x00,0x00,0xba,0x55,0x00,0x40,0x40,0x20,0x60,0x60,0x48,0x18,0x8c,0x46,0x23},
  {0x69,0x69,0x2f,0x3a,0x23,0x1a,0x1a,0x0d,0x04,0x04,0x03,0x01,0x00,0x00,0x00},
  {   1,   0,   1,   0,   1,   1,   0,   0,   1,   0,   0,   1,   1,   1,   1}
};

static void sio_intr (int vec, void *data, struct pt_regs *fp);
static void sio12_intr (int vec, void *data, struct pt_regs *fp);
static void do_sio_intr (struct async_struct *info);
static void sio_tx_int (int vec, void *data, struct pt_regs *fp);

static void sio_init (struct async_struct *info);
static void sio_deinit (struct async_struct *info, int leave_dtr);
static void sio_enab_tx_int (struct async_struct *info, int enab_flag);
static int  sio_check_custom_divisor (struct async_struct *info,
				      int baud_base, int divisor);
static void sio_change_speed (struct async_struct *info);
static void sio_throttle (struct async_struct *info, int status);
static void sio_set_break (struct async_struct *info, int break_flag);
static void sio_get_serial_info (struct async_struct *info,
				struct serial_struct *retinfo);
static unsigned int sio_get_modem_info (struct async_struct *info);
static int sio_set_modem_info (struct async_struct *info, int new_dtr,
			      int new_rts);
static int sio_ioctl (struct tty_struct *tty, struct file *file,
		       struct async_struct *info, unsigned int cmd,
						    unsigned long arg);
static void sio_stop_receive (struct async_struct *info);
static int sio_trans_empty (struct async_struct *info);


/*  SERIALSWITCH structure for the sio mc68561 ports.  */

static SERIALSWITCH sio_switch = {
	sio_init, sio_deinit, sio_enab_tx_int,
	sio_check_custom_divisor, sio_change_speed,
	sio_throttle, sio_set_break,
	sio_get_serial_info, sio_get_modem_info,
	sio_set_modem_info, sio_ioctl, sio_stop_receive,
	sio_trans_empty, NULL
};

static int num_sio = 0;


/*         Initialize routine.                  */

int cp31_sioinit (struct async_struct *info, int rs_count) {
	volatile struct sio *v;
	volatile struct bim *bim = (struct bim *) BIM_ADDR;
	int vector, level;
	int serial_type = 1000;
	unsigned char old, new;
	unsigned int value;

#ifdef CONFIG_PROC_FS
	serial_type = besta_add_serial_type ("sio (cp31)", 0);
	if (serial_type < 0)  serial_type = 1000;
#endif


/*      Initialize first sio (console).    */
	v = (struct sio *) SIO_ADDR;
	info = &info[rs_count++];

	info->type = serial_type;
	info->port = SIO_ADDR;
	info->sw = &sio_switch;
	info->IER = 0;

	if (!besta_get_vect_lev ("sio", &vector, &level)) {
	    vector = get_unused_vector();
	    level = SIO_LEV;
	}

	v->s_ena = 0x0;  /*  Ser int off  */
	v->t_ena = 0;    /*  dis Tx  */
	v->r_ena = 0;    /*  dis Rx  */

	besta_handlers[vector] = sio_intr;
	besta_intr_data[vector] = info;

	bim->vect[1].reg = vector;
	bim->cntrl[1].reg = level | 0x10;

	num_sio++;


/*   Initialize second sio (if any, on slave cp20 board etc...  )  */

	v = (struct sio *) SIO1_ADDR;
	if (VME_probe (&v->r_vect, 0, PROBE_READ, PORT_BYTE))  return rs_count;

	/*   SIO1_ADDR == CEN_ADDR . So, we must do some test to decide
	   what device is there.
	     sio->r3 has offset of pit->b_dir .
	     sio->r3 is an unused port and returns (if there is no bus error)
	   a value of 0xff (or another gabage?).
	     pit->b_dir may store data, and we can assume there are no
	   any side effects, because pit port B is not used.
	     So, we try to write, read and compare. If OK, assume
	   it is mc68230 pit, else continue check for mc68561 sio.
	*/

	if (VME_probe (&v->r3, &value, PROBE_READ, PORT_BYTE) == 0) {
	    old = value;
	    new = old ^ 0x01;
	    value = new;
	    if (VME_probe (&v->r3, &value, PROBE_WRITE, PORT_BYTE) == 0 &&
		v->r3 == new            /*  i.e., can write and store   */
	    ) {
		v->r3 = old;    /*  restore `right' value   */
		return rs_count;    /*  no more sio, this is pit   */
	    }
	}

	/*  Because successful read is not enough test,
	   `She  wants  more,  more,  more, '
	   If sio present, it must store data in sio->r_vect port.
	   So, test for it.
	*/
	old = v->r_vect;
	v->r_vect = old ^ 0x1;
	new = v->r_vect;
	v->r_vect = old;
	if (new != (old ^ 0x1))  return rs_count;

	info = &info[rs_count++];

	info->type = serial_type;
	info->port = SIO1_ADDR;
	info->sw = &sio_switch;
	info->IER = 0;

	v->r_cntl = 0x1;   /*  Rx reset  */
	v->t_cntl = 0x1;   /*  Tx reset  */
	v->s_ena = 0x0;  /*  Ser int off  */

	v->t_ena = 0;    /*  dis Tx  */
	v->r_ena = 0;    /*  dis Rx  */

	if (!besta_get_vect_lev ("sio1", &vector, &level)) {
	    vector = get_unused_vector();
	    level = SIO1_LEV;
	}

	besta_handlers[vector] = sio12_intr;
	besta_intr_data[vector] = info;     /*  but info for first only  */

	bim->vect[3].reg = vector;
	bim->cntrl[3].reg = level | 0x10;

	num_sio++;

/*   Initialize third sio (if any, on addition sp500 board etc...  )  */

	v = (struct sio *) SIO2_ADDR;
	if (VME_probe (&v->r_vect, 0, PROBE_READ, PORT_BYTE))  return rs_count;

	/*  Because successful read is not enough test,
	   `She  wants  more,  more,  more, '
	   If sio present, it must store data in sio->r_vect port.
	   So, test for it.
	*/
	old = v->r_vect;
	v->r_vect = old ^ 0x1;
	new = v->r_vect;
	v->r_vect = old;
	if (new != (old ^ 0x1))  return rs_count;

	info = &info[rs_count++];

	info->type = serial_type;
	info->port = SIO2_ADDR;
	info->sw = &sio_switch;
	info->IER = 0;

	v->r_cntl = 0x1;   /*  Rx reset  */
	v->t_cntl = 0x1;   /*  Tx reset  */
	v->s_ena = 0x0;  /*  Ser int off  */

	v->t_ena = 0;    /*  dis Tx  */
	v->r_ena = 0;    /*  dis Rx  */

	/*  the same interrupt stuff as for second sio port   */

	num_sio++;

	return  rs_count;
}


static void sio_change_speed (struct async_struct *info) {
	volatile struct sio *v = (struct sio *) info->port;
	unsigned short cflag;
	unsigned char prot_sel0, prot_sel1, err_cntl, baud;
	unsigned short flags;

	if (!info->tty || !info->tty->termios)  return;

	cflag = info->tty->termios->c_cflag;

	if (!((cflag ^ info->IER) & (CBAUD|CSIZE|CSTOPB|PARENB|PARODD)))
		return;         /*  nothing to do   */

	if ((cflag & CBAUD) == B0) {
	    v->t_ena = 0;    /*  dis Tx  */
	    v->r_ena = 0;    /*  dis Rx  */

	    return;
	}

	prot_sel0 = 0x0;     /*  byte mode   */

	prot_sel1 = 0x06;    /*  async mode   */
	switch (cflag & CSIZE) {
	    case CS5:  prot_sel1 |= 0x00;  break;
	    case CS6:  prot_sel1 |= 0x08;  break;
	    case CS7:  prot_sel1 |= 0x10;  break;
	    case CS8:  prot_sel1 |= 0x18;  break;
	}
	if (cflag & CSTOPB)  prot_sel1 |= 0x40;

	err_cntl = 0;
	if (cflag & PARENB)  err_cntl |= 0x80;
	if (cflag & PARODD)  err_cntl |= 0x40;

	baud = (cflag & CBAUD) - 1;

	save_flags (flags);
	cli();

	v->r_ena = 0;    /*  dis Rx   */
	v->t_ena = 0;    /*  dis Tx   */

	v->prot_sel0 = prot_sel0;
	v->prot_sel1 = prot_sel1;
	v->err_cntl = err_cntl;

	v->baud_div0 = sio_magics[0][baud];
	v->baud_div1 = sio_magics[1][baud];
	v->clock_cntl = (sio_magics[2][baud] << 4) | 0x0c;

	v->s_cntl = 0x40 | 0x80;  /*  DTR on, RTS on   */

	v->r_cntl = 0x0;   /*  Rx on   */
	v->t_cntl = 0x80;  /*  Tx on   */

	v->r_ena = 0x80; /*  Rx ena   */
	v->t_ena = 0x80; /*  Tx ena   */

	info->IER = cflag;      /*  store old values...  */

	if (cflag & CRTSCTS)  info->flags |= ASYNC_CTS_FLOW;
	else  info->flags &= ~ASYNC_CTS_FLOW;

	if (cflag & CLOCAL)  info->flags &= ~ASYNC_CHECK_CD;
	else  info->flags |= ASYNC_CHECK_CD;

	restore_flags (flags);

	return;
}

static void sio_init (struct async_struct *info) {
	volatile struct sio *v = (struct sio *) info->port;

	v->r_cntl = 0x00;  /*  Rx on   */
	v->t_cntl = 0x80;  /*  Tx on   */

	v->r_ena = 0x80; /*  Rx ena   */
	v->t_ena = 0x80; /*  Tx ena   */

	v->s_cntl = 0x40 | 0x80;  /*  DTR on, RTS on   */

	return;
}

static void sio_deinit (struct async_struct *info, int leave_dtr) {
	volatile struct sio *v = (struct sio *) info->port;

	v->r_ena = 0;    /*  dis Rx   */
	v->t_ena = 0;    /*  dis Tx   */

#if 0
	while (!(v->t_stat & 0x80)) ;

	/*  0, because on reset extra chars go into the line...  */
	v->r_cntl = 0x1;   /*  Rx reset   */
	v->t_cntl = 0x1;   /*  Tx reset   */
#endif

	if (leave_dtr)  v->s_cntl = 0x40;     /*  RTS off   */
	else  v->s_cntl = 0;      /*  DTR off, RTS off   */

	return;
}

static void sio_enab_tx_int (struct async_struct *info, int enab_flag) {
	volatile struct sio *v = (struct sio *) info->port;
#if 0
	unsigned short flags;
#endif

	if (enab_flag) {
	    v->t_ena = 0x80;     /*  Tx ena   */

#if 0
	    save_flags (flags);
	    cli();
	    sio_tx_int (0, info, 0);
	    restore_flags (flags);
#endif

	} else
	    v->t_ena = 0;        /*  dis Tx   */

	return;
}

static int  sio_check_custom_divisor (struct async_struct *info,
				      int baud_base, int divisor) {

	/*  but may be possible to implement somewhat here...  */

	return -1;
}

static void sio_throttle (struct async_struct *info, int status) {
	volatile struct sio *v = (struct sio *) info->port;

	if (status)  v->s_cntl &= ~0x80;  /*  RTS off   */
	else  v->s_cntl |= 0x80;      /*  RTS on   */

	return;
}

static void sio_set_break (struct async_struct *info, int break_flag) {
	volatile struct sio *v = (struct sio *) info->port;

	if (break_flag)  v->t_cntl |= 0x20;    /*  set break   */
	else  v->t_cntl &= ~0x20;

	return;
}

static void sio_get_serial_info (struct async_struct *info,
				struct serial_struct *retinfo) {

	retinfo->baud_base = info->baud_base;
	retinfo->custom_divisor = info->custom_divisor;

	return;
}

static unsigned int sio_get_modem_info (struct async_struct *info) {
	volatile struct sio *v = (struct sio *) info->port;

	return  ((v->s_cntl & 0x80) ? TIOCM_RTS : 0) |
		((v->s_cntl & 0x40) ? TIOCM_DTR : 0) |
		TIOCM_CAR |     /*  how???...   */
		TIOCM_CTS |     /*  how???...   */
		TIOCM_DSR;
}

static int sio_set_modem_info (struct async_struct *info, int new_dtr,
							    int new_rts) {
	volatile struct sio *v = (struct sio *) info->port;

	if (new_dtr)  v->s_cntl |= 0x40;
	else  v->s_cntl &= ~0x40;

	if (new_rts)  v->s_cntl |= 0x80;
	else  v->s_cntl &= ~0x80;

	return 0;
}

static void sio_stop_receive (struct async_struct *info) {
	volatile struct sio *v = (struct sio *) info->port;

	v->r_ena = 0;    /*  dis Rx   */
#if 0
	/*  0, because on reset extra chars go into the line...  */
	v->r_cntl = 0x01;  /*  Rx reset   */
#endif

	return;
}

static int sio_trans_empty (struct async_struct *info) {
	volatile struct sio *v = (struct sio *) info->port;

	return  (v->t_stat & 0x80) != 0;
}

static void sio_tx_int (int vec, void *data, struct pt_regs *fp) {
	struct async_struct *info = data;
	volatile struct sio *v = (struct sio *) info->port;

	while (v->t_stat & 0x80) {     /*  Tx ready   */
	    int ch;

	    ch = rs_get_tx_char (info);
	    if (ch >= 0)  v->t_data = ch;
	    else {
		v->t_ena = 0;    /*  dis  Tx   */
		break;
	    }
	}

	return;
}

/*    Various interrupt routines...   */

static void sio_intr (int vec, void *data, struct pt_regs *fp) {
	struct async_struct *info = data;

	do_sio_intr (info);

	return;
}

static void sio12_intr (int vec, void *data, struct pt_regs *fp) {
	struct async_struct *info = data;
	volatile struct pit *pit = (struct pit *) PIT_ADDR;

	/*  reset interrupt status on mc68230   */
	pit->status = 0x08;

	if (num_sio < 2)  return;

	do {
	    do_sio_intr (info);

	    if (num_sio == 3 && (pit->status & 0x80) == 0)
		do_sio_intr (info + 1);     /*  third sio may be   */

	} while ((pit->status & 0x80) == 0);
			/*  is an infinitive loop possible hear ???  */

	return;
}

static void do_sio_intr (struct async_struct *info) {
	volatile struct sio *v = (struct sio *) info->port;
	int stat = 0;

	if (info && info->flags & ASYNC_INITIALIZED) {

	    while (v->r_stat & 0x80) {
		int ch = v->r_data;
		int err = 0;

		stat = v->r_stat & 0x1e;
		if (stat) {     /*  Rx error   */

		    if (stat & 0x10)  err = TTY_PARITY;
		    else if (stat & 0x8)  err = TTY_FRAME;
		    else if (stat & 0x4)  err = TTY_OVERRUN;
		    else  err = TTY_BREAK;
		}

		rs_receive_char (info, ch, err);
	    }

	    if (stat)  v->r_stat = stat;    /*  reset somewhat here  */

	    sio_tx_int (0, info, 0);
	}

	return;
}


/* Routine for kernel messages onto console  */
void sio_putstring (const char *s) {
	volatile struct sio *v = (struct sio *) SIO_ADDR;
	unsigned short flags;
	char c;
	unsigned char t_ena;

	save_flags (flags);
	cli();

	t_ena = v->t_ena;
	v->t_ena = 0;    /*  dis  Tx   */

	while((c = *s++) != 0) {
		int delay = 200000;     /*  hoping it is enough...   */

		while (!(v->t_stat & 0x80) && delay-- > 0) ;
		if (delay <= 0)  break;    /*  foo on you   */

		if (c == '\n') {
		    v->t_data = '\r';

		    delay = 200000;
		    while (!(v->t_stat & 0x80) && delay-- > 0) ;
		    if (delay <= 0)  break;    /*  foo on you   */
		}
		v->t_data = c;
	}

	v->t_ena = t_ena;

	restore_flags (flags);

	return;
}


/*  Someone use /dev/console to access hardware clock. Be-e-e-e-e!!!
   Sorry. They developed the design without me...
*/
static int sio_ioctl (struct tty_struct *tty, struct file *file,
		       struct async_struct *info, unsigned int cmd,
						    unsigned long arg) {
	int err;
	struct hwclk_time time;

	/*  only console dev allow it   */
	if (info->port != SIO_ADDR)  return -ENOIOCTLCMD;

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

