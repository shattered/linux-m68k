/*
 * besta/hcww.c -- Low level serial driver and centronics write-only driver
 *		   for HCWW8 VME-board. Up to 8 serial ports,
 *		   optional centronics, optional real time clock.
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

struct hcww_board {
	union {
	    struct board {
		char          r0[3];
		unsigned char vst;
	    } board;
	    struct acia {
		char          r0[5];
		unsigned char cntst;
		char          r6;
		unsigned char data;
	    } acia[8];
	    struct baud {
		char          r0;
		unsigned char baud;
		char          r2[14];
	    } baud[4];
	} u;
	struct hclk {
	    char r0; unsigned char sec100;
	    char r2; unsigned char hour;
	    char r4; unsigned char minute;
	    char r6; unsigned char second;
	    char r8; unsigned char month;
	    char ra; unsigned char day;
	    char rc; unsigned char year;
	    char re; unsigned char week;
	    char r10; unsigned char rsec100;
	    char r12; unsigned char rhour;
	    char r14; unsigned char rminute;
	    char r16; unsigned char rsecond;
	    char r18; unsigned char rmonth;
	    char r1a; unsigned char rday;
	    char r1c; unsigned char ryear;
	    char r1e; unsigned char rweek;
	    char r20; unsigned char status;
	    char r22; unsigned char cmd;
	} hclk;
	char r64[28];
	struct hcen {
	    char          r0;
	    unsigned char cntrl;
	    char          r1;
	    unsigned char data;
	} hcen;
};

static int hcww_cnt = 0;

static int hcen_open (struct inode *inode, struct file *filp);
static void hcen_release (struct inode *inode, struct file *filp);
static int hcen_read (struct inode *inode, struct file *filp,
						char *buf, int count);
static int hcen_write (struct inode *inode, struct file *filp,
					    const char * buf, int count);
static int hcen_ioctl (struct inode *inode, struct file *filp,
				unsigned int cmd, unsigned long arg);

static struct file_operations hcen_fops = {
	NULL,
	hcen_read,
	hcen_write,
	NULL,
	NULL,
	hcen_ioctl,
	NULL,
	hcen_open,
	hcen_release,
	NULL,
	NULL,
	NULL,
	NULL,
};

static int hcen_registered = 0;

static struct hcww_info {
	int     board_base;
	struct async_struct *serial_info_base;
	char    serials;
	char    clock;
	char    centronics;
	char    hcen_state;
	struct wait_queue *hcen_wait;
} hcww_info[MAX_SERIAL/8] = { { 0, }, };

/*  values for hcen_state   */
#define STATE_FREE      0
#define STATE_READY     1
#define STATE_WAIT      2
#define STATE_BREAK     3

static void hcww_intr (int vec, void *data, struct pt_regs *fp);

static void acia_tx_int (int vec, void *data, struct pt_regs *fp);
static void acia_init (struct async_struct *info);
static void acia_deinit (struct async_struct *info, int leave_dtr);
static void acia_enab_tx_int (struct async_struct *info, int enab_flag);
static int  acia_check_custom_divisor (struct async_struct *info,
				      int baud_base, int divisor);
static void acia_change_speed (struct async_struct *info);
static void acia_throttle (struct async_struct *info, int status);
static void acia_set_break (struct async_struct *info, int break_flag);
static void acia_get_serial_info (struct async_struct *info,
				struct serial_struct *retinfo);
static unsigned int acia_get_modem_info (struct async_struct *info);
static int acia_set_modem_info (struct async_struct *info, int new_dtr,
			      int new_rts);
static void acia_stop_receive (struct async_struct *info);
static int acia_trans_empty (struct async_struct *info);

static SERIALSWITCH acia_switch = {
    acia_init, acia_deinit, acia_enab_tx_int,
    acia_check_custom_divisor, acia_change_speed,
    acia_throttle, acia_set_break,
    acia_get_serial_info, acia_get_modem_info,
    acia_set_modem_info, NULL, acia_stop_receive, acia_trans_empty, NULL
};

static unsigned char acia_magics[16] = {
	0, 0,0x09,0x0d, 0, 0,0x01,0x05,0x15,0x15,0x19,0x1d,0x11,0x11, 0, 0
};
static unsigned char acia_speed[15] = {
	20, 15, 14, 20, 13, 20, 12, 11, 4, 10, 3, 9, 8, 1, 0
};


void hcww_init (struct VME_board *VME, int on_off) {
	int i, hcen_present = 0, hclk_present = 0;
	volatile struct hcww_board *hcww = (struct hcww_board *) VME->addr;
	struct async_struct *info;
	int serial_type = 1000;
	int vector;

	if (on_off) {       /*  deinit   */
	    int i = VEC_UNINT;

	    /*  no way to intr off instead of reset ???   */
	    VME_probe (&hcww->u.board.vst, &i, PROBE_WRITE, PORT_BYTE);

	    return;
	}


	if (VME_probe (&hcww->u.baud[0].baud, 0, PROBE_READ, PORT_BYTE)) {
		printk ("    no %s at 0x%08x\n",
			(VME->name ? VME->name : "board"), VME->addr);
		return;
	}

	vector = (VME->vect < 0) ? get_unused_vector() : VME->vect;

	if (besta_handlers[vector] != NULL) {
		printk ("Cannot register hcww board at 0x%08x: "
			"vector 0x%x is already used\n", VME->addr, vector);
		return;
	}

	VME->present = 1;       /*  OK, board is present   */

#ifdef CONFIG_PROC_FS
	serial_type = besta_add_serial_type ("acia (hcww)", 0);
	if (serial_type < 0)  serial_type = 1000;
#endif

	for (i = 0; VME_serial_cnt + i < MAX_SERIAL && i < 8; i++) {

	    hcww->u.acia[i].cntst = 0x3;    /* reset  */

	    info = &rs_table[VME_serial_cnt + i];
	    info->type = serial_type;
	    info->port = (int) &hcww->u.acia[i];
	    info->board_base = (void *) VME->addr;
	    info->sw = &acia_switch;
	    info->IER = 0;
	    info->MCR = 0;
	}

	if (VME_probe (&hcww->hcen.cntrl, 0, PROBE_READ, PORT_BYTE) == 0) {
	    hcen_present = 1;
	    hcww_info[hcww_cnt].hcen_state = STATE_FREE;
	    if (!hcen_registered) {
		hcen_registered = 1;
		if (register_chrdev (VME->major, "hcen", &hcen_fops))
		    printk ("Unable to get major %d (hcww)\n", VME->major);
	    }
	}

	if (VME_probe (&hcww->hclk.ryear, 0, PROBE_READ, PORT_BYTE) == 0) {
	    /*  Some test if we really have a clock on this board.  */
	    hcww->hclk.status = 0;
	    hcww->hclk.cmd = 0x04;

	    hcww->hclk.sec100 = 0;
	    hcww->hclk.hour = 0;
	    hcww->hclk.minute = 0;
	    hcww->hclk.second = 0;
	    hcww->hclk.month = 1;
	    hcww->hclk.day = 1;
	    hcww->hclk.year = 1;

	    hcww->hclk.cmd = 0x0c;
	    if (hcww->hclk.sec100 == 0) {
		int start = jiffies;

		while (jiffies < start + 2) ;
		if (hcww->hclk.sec100 > 0)  hclk_present = 1;
	    }

	    hcww->hclk.cmd = 0;
	}

	/*  OK   */

	printk ("  0x%08x: HCWW8", VME->addr);
	if (!hclk_present || !hcen_present) {
	    printk ("-");
	    if (!hclk_present)  printk ("R");
	    if (!hcen_present)  printk ("C");
	}
	printk (", 8 serials ");
	if (i)  printk ("(%d,%d-%d) ", TTY_MAJOR,
				       SERIAL_START + VME_serial_cnt,
				       SERIAL_START + VME_serial_cnt + i - 1);
	else  printk ("(not used) ");
	if (hcen_present)  printk ("centronics (%d,%d) ",
						VME->major, hcww_cnt);
	if (hclk_present)  printk ("RTC (not used)");
	printk ("\n");

	hcww_info[hcww_cnt].board_base = VME->addr;
	hcww_info[hcww_cnt].serial_info_base = &rs_table[VME_serial_cnt];
	hcww_info[hcww_cnt].serials = i;
	hcww_info[hcww_cnt].centronics = hcen_present;
	hcww_info[hcww_cnt].clock = hclk_present;

	besta_handlers[vector] = hcww_intr;
	besta_intr_data[vector] = &hcww_info[hcww_cnt];
	hcww->u.board.vst = vector;

	VME_serial_cnt += i;
	hcww_cnt++;

	return;
}

/*      centronics stuff        */

static int hcen_open (struct inode *inode, struct file *filp) {
	int minor = MINOR (inode->i_rdev);
	unsigned short flags;

	if (minor >= hcww_cnt || hcww_info[minor].centronics == 0)
							    return -ENXIO;
	save_flags (flags);
	cli();

	if (hcww_info[minor].hcen_state != STATE_FREE) {
		restore_flags (flags);
		return -EBUSY;
	}

	hcww_info[minor].hcen_state = STATE_READY;

	restore_flags (flags);

	return 0;
}

static void hcen_release (struct inode *inode, struct file *filp) {
	int minor = MINOR (inode->i_rdev);

	hcww_info[minor].hcen_state = STATE_FREE;

	return;
}

static int hcen_read (struct inode *inode, struct file *filp,
						char *buf, int count) {
	return -ENOSYS;

}

static int hcen_write (struct inode *inode, struct file *filp,
					    const char * buf, int count) {
	int minor = MINOR (inode->i_rdev);
	unsigned short flags;
	int err, n = count;
	volatile struct hcww_board *hcww =
			    (struct hcww_board *) hcww_info[minor].board_base;

	err = verify_area (VERIFY_READ, buf, count);
	if (err)  return err;

	save_flags (flags);
	cli();

	if ((hcww->hcen.cntrl & 0x04) == 0) {   /*  SLCT no  */
	    restore_flags (flags);
	    return -EIO;
	}

	while (n > 0) {
	    unsigned char ch;

	    ch = get_fs_byte (buf);
	    hcww->hcen.data = ch;
	    hcww->hcen.cntrl = 0x48;
	    hcww->hcen.cntrl = 0x40;

	    hcww_info[minor].hcen_state = STATE_WAIT;
	    interruptible_sleep_on (&hcww_info[minor].hcen_wait);
	    if (hcww_info[minor].hcen_state != STATE_READY) {
		hcww_info[minor].hcen_state = STATE_BREAK;
		count = -EINTR;
		break;
	    }

	    n--;
	    buf++;
	}

	hcww->hcen.cntrl = 0;

	restore_flags (flags);

	return count;
}

static int hcen_ioctl (struct inode *inode, struct file *filp,
				unsigned int cmd, unsigned long arg) {
	return -ENOSYS;

}


/*      serial stuff       */

static void acia_change_speed (struct async_struct *info) {
	volatile struct hcww_board *hcww =
				    (struct hcww_board *) info->board_base;
	volatile struct acia *v = (struct acia *) info->port;
	unsigned int new_cflag;
	int n, baud, baud1, num;
	unsigned short flags;

	if (!info->tty || !info->tty->termios)  return;
	new_cflag = info->tty->termios->c_cflag;

	if ((new_cflag & CBAUD) == B0) {
	    v->cntst = info->IER &= 0x1f;   /* dis Tx, dis Rx */
	    info->MCR = new_cflag;
	    return;
	}

	if (!((new_cflag ^ info->MCR) &
		(CBAUD | CSIZE | CSTOPB | PARENB | PARODD | CRTSCTS | CLOCAL))
	)  return;      /*  the same hardware parameters...  */

	n = 0;
	if ((new_cflag & CSIZE) == CS8)  n |= 0x8;
	if (new_cflag & CSTOPB)  n |= 0x4;
	if (new_cflag & PARENB)  n |= 0x2;
	if (new_cflag & PARODD)  n |= 0x1;

	n = acia_magics[n];
	if (!n)  return;

	baud = acia_speed[(new_cflag & CBAUD) - 1];
	if (baud > 15)  return;
	if (baud & 0x8)  n++;

	save_flags (flags);
	cli();

	n |= info->IER & 0xe0;

	if (new_cflag & CRTSCTS)
		info->flags |= ASYNC_CTS_FLOW;
	else
		info->flags &= ~ASYNC_CTS_FLOW;
	if (new_cflag & CLOCAL)
		info->flags &= ~ASYNC_CHECK_CD;
	else
		info->flags |= ASYNC_CHECK_CD;

	/*  setting...  */

	if (n != info->IER)  v->cntst = info->IER = n;

	if (!((info->MCR ^ new_cflag) & CBAUD))
		goto  exit_change;

	num = v - &hcww->u.acia[0];

	if (num % 1) {

	    baud1 = ((info-1)->tty && (info-1)->tty->termios)
		    ? acia_speed[((info-1)->tty->termios->c_cflag & CBAUD) - 1]
		    : 0;
	    if (baud1 > 15)  baud1 = 0;

	    baud = (baud1 << 4) | (baud & 0xf);

	} else {

	    baud1 = ((info+1)->tty && (info+1)->tty->termios)
		    ? acia_speed[((info+1)->tty->termios->c_cflag & CBAUD) - 1]
		    : 0;
	    if (baud1 > 15)  baud1 = 0;

	    baud = (baud << 4) | (baud1 & 0xf);
	}

	/*  setting   */

	hcww->u.baud[num>>1].baud = baud;

exit_change:
	info->MCR = new_cflag;

	restore_flags (flags);

	return;
}

static void acia_init (struct async_struct *info) {
	volatile struct acia *v = (struct acia *) info->port;

	v->cntst = info->IER = (info->IER & 0x1f) | 0xa0;   /* ena Tx, ena Rx */

	return;
}

static void acia_deinit (struct async_struct *info, int leave_dtr) {
	volatile struct acia *v = (struct acia *) info->port;

	v->cntst = info->IER &= 0x1f;   /* dis Tx, dis Rx  */

	return;
}

static void acia_enab_tx_int (struct async_struct *info, int enab_flag) {
	volatile struct acia *v = (struct acia *) info->port;
	unsigned short flags;

	if (enab_flag) {
	    v->cntst = info->IER = (info->IER & 0x9f) | 0x20;  /* ena Tx  */

	    save_flags (flags);
	    cli();
	    acia_tx_int (0, info, 0);
	    restore_flags (flags);
	} else
	    v->cntst = info->IER &= 0x9f;   /* dis Tx  */

	return;
}

static int acia_check_custom_divisor (struct async_struct *info,
					int baud_base, int divisor) {
	return -1;

}

static void acia_throttle (struct async_struct *info, int status) {

	/*  nothing should be hear or reset ???   */
	return;
}

static void acia_set_break (struct async_struct *info, int break_flag) {
	volatile struct acia *v = (struct acia *) info->port;

	if (break_flag)
	    v->cntst = info->IER = (info->IER & 0x9f) | 0x60;
	else
	    v->cntst = info->IER &= 0x9f;   /* dis Tx  */

	return;
}

static void acia_get_serial_info (struct async_struct *info,
					struct serial_struct *retinfo) {

	retinfo->baud_base = info->baud_base;
	retinfo->custom_divisor = info->custom_divisor;

	return;
}

static unsigned int acia_get_modem_info (struct async_struct *info) {

	return (TIOCM_RTS | TIOCM_DTR | TIOCM_CAR | TIOCM_CTS | TIOCM_DSR);

}

static int acia_set_modem_info (struct async_struct *info,
						int new_dtr, int new_rts) {
	return -ENOSYS;

}

static void acia_stop_receive (struct async_struct *info) {
	volatile struct acia *v = (struct acia *) info->port;

	v->cntst = info->IER &= 0x7f;   /* dis Rx  */

	return;
}

static int acia_trans_empty (struct async_struct *info) {
	volatile struct acia *v = (struct acia *) info->port;

	return (v->cntst & 0x2) != 0;   /*  Tx ready   */
}

static void acia_tx_int (int vec, void *data, struct pt_regs *fp) {
	struct async_struct *info = data;
	volatile struct acia *v = (struct acia *) info->port;

	while (v->cntst & 0x2) {    /*  Tx ready   */
	    int ch;

	    if ((ch = rs_get_tx_char (info)) >= 0)  v->data = ch;
	    else {
		v->cntst = info->IER &= 0x9f;   /* dis Tx  */
		break;
	    }
	}

	return;
}

static void hcww_intr (int vec, void *data, struct pt_regs *fp) {
	struct hcww_info *hcww_info = data;
	volatile struct hcww_board *hcww =
				(struct hcww_board *) hcww_info->board_base;
	struct async_struct *info;
	int n, stat;

	n = hcww->u.board.vst & 0x0f;

	if (n & 0x8) {  /*  centronics or no interrupt   */

	    if (hcww_info->centronics &&
		(hcww->hcen.cntrl & 0xc0) == 0xc0
	    ) {
		hcww->hcen.cntrl = 0;

		if (hcww_info->hcen_state != STATE_WAIT)  return;
		wake_up_interruptible (&hcww_info->hcen_wait);
		hcww_info->hcen_state = STATE_READY;

		return;
	    }
	}

	n &= 0x7;

	if (n == 0 && hcww_info->clock &&
		      hcww->hclk.status & 0x80) {
	    printk ("hcww at 0x%08x: clock uninitialized interrupt"
				    " (ignored)\n", hcww_info->board_base);
	}

	do {
	    if (n >= hcww_info->serials)  continue;

	    info = hcww_info->serial_info_base + n;

	    if (info && info->flags & ASYNC_INITIALIZED) {

		while ((stat = hcww->u.acia[n].cntst) & 0x1) {  /* Rx ready */
		    int ch = hcww->u.acia[n].data;
		    int err = 0;

		    if ((stat &= 0x78) != 0) {  /*  Rx error  */
			if (stat & 0x10)  err = TTY_BREAK;
			else if (stat & 0x20)  err = TTY_OVERRUN;
			else if (stat & 0x40)  err = TTY_PARITY;
			else  err = TTY_FRAME;
		    }

		    rs_receive_char (info, ch, err);
		}

		acia_tx_int (vec, info, fp);
	    }

	} while ((n = hcww->u.board.vst & 0x0f) < 8);

	return;
}
