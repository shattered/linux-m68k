/*
 *  linux/drivers/char/m68kserial.c
 *
 *
 * Copyright 1994 Roman Hodek <Roman.Hodek@informatik.uni-erlangen.de>
 *
 * Partially based on PC-Linux serial.c by Linus Torvalds and Theodore Ts'o
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file COPYING in the main directory of this archive
 * for more details.
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *
 *  Extensively rewritten by Theodore Ts'o, 8/16/92 -- 9/14/92.  Now
 *  much more extensible to support other serial cards based on the
 *  16450/16550A UART's.  Added support for the AST FourPort and the
 *  Accent Async board.  
 *
 *  set_serial_info fixed to set the flags, custom divisor, and uart
 * 	type fields.  Fix suggested by Michael K. Johnson 12/12/92.
 *
 * This module exports the following rs232 io functions:
 *
 *	long rs_init(long);
 * 	int  rs_open(struct tty_struct * tty, struct file * filp)
 */

/*
 * Notes and Design Goals:
 * -----------------------
 * The PC serial drivers can rely on the fact that all the serial
 * hardware is very similar to program for all ports. Unfortunately,
 * this is not true for m68k machines, especially the Atari. Here it is
 * nearly the other way 'round: All ports need different treatment for
 * the low-level stuff.
 *
 * For this reason, I've split the serial driver code into a
 * port-independent part (serial.c) and port-specific parts (atari_*.c,
 * ...). The first manages all what can be done without accessing the
 * hardware directly, i.e. interfacing with the high-level tty drivers,
 * wait queues, managing existing ports and the like. The latter do the
 * actual hardware programming and are accessed by the hardware
 * independent part by a "switch" structure, that contains pointers to
 * functions for specific tasks. See the comment before the definition
 * of the SERIALSWITCH structure in <linux/serial.h> for more details.
 *
 * The port-independent code should be usable by other machines than
 * m68k ones, too, if there are similar circumstances with different
 * serial port hardware. Feel free to use it, but please inform me if
 * you have to do changes to it. I'll try to keep it really
 * device-independent.
 *
 * Roman <Roman.Hodek@informatik.uni-erlangen.de>
 */

#include <linux/module.h>

#include <linux/types.h>
#include <linux/errno.h>
#include <linux/signal.h>
#include <linux/sched.h>
#include <linux/timer.h>
#include <linux/interrupt.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>
#include <linux/serial.h>
#include <linux/config.h>
#include <linux/major.h>
#include <linux/string.h>
#include <linux/fcntl.h>
#include <linux/ptrace.h>
#include <linux/mm.h>
#include <linux/proc_fs.h>

#include <asm/setup.h>
#include <asm/system.h>
#include <asm/io.h>
#include <asm/segment.h>
#include <asm/bitops.h>

#ifdef CONFIG_ATARI
#include "atari_SCC.h"
#include "atari_MFPser.h"
#include "atari_MIDI.h"
#endif

#ifdef CONFIG_KERNELD
#include <linux/kerneld.h>
#endif

#ifdef CONFIG_AMIGA
int amiga_serinit (void);

#ifdef CONFIG_GVPIOEXT
int ioext_init (void);
#endif

#ifdef CONFIG_MULTIFACE_III_TTY
int multiface_init(void);
#endif

#ifdef CONFIG_WHIPPET
int whippet_init (void);
#endif

#ifdef CONFIG_ASDG_DSB
int asdg_setup(void);
#endif
#endif

DECLARE_TASK_QUEUE(tq_serial);

struct tty_driver serial_driver, callout_driver;
static int serial_refcount;

/* serial subtype definitions */
#define SERIAL_TYPE_NORMAL	1
#define SERIAL_TYPE_CALLOUT	2

/* number of characters left in xmit buffer before we ask for more */
#define WAKEUP_CHARS 256

#define SERIAL_PARANOIA_CHECK
#define CONFIG_SERIAL_NOPAUSE_IO
#define SERIAL_DO_RESTART

#undef SERIAL_DEBUG_INTR
#undef SERIAL_DEBUG_OPEN
#undef SERIAL_DEBUG_FLOW

#define _INLINE_ inline

#ifdef CONFIG_BESTA
#define NR_PORTS	64
#else
#define NR_PORTS	6
#endif

struct async_struct rs_table[NR_PORTS];
static struct tty_struct *serial_table[NR_PORTS];
static struct termios *serial_termios[NR_PORTS];
static struct termios *serial_termios_locked[NR_PORTS];

#ifndef MIN
#define MIN(a,b)	((a) < (b) ? (a) : (b))
#endif

static char *serialtypes[] = {
	"unknown", "8250", "16450", "16550", "16550A"
};
#define PORT_MAX (sizeof(serialtypes)/sizeof(*serialtypes))
#ifdef __mc68000__
static char *serialtypes68k[] = {
	"SCC w/o DMA", "SCC w/ DMA",
	"MFP", "MFP w/o ctrl lines",
	"MIDI",
	"Amiga builtin", "GVP IO-Extender (16c552)", "BSC MultiFaceCard III",
	"Hisoft Whippet", "ASDG DSB"
};
#define M68K_PORT_MAX (sizeof(serialtypes68k)/sizeof(*serialtypes68k))
#endif

#ifndef MODULE
#ifdef CONFIG_PROC_FS
static int proc_serial_read (char *buffer, char **start, off_t offset,
			     int size, int unused)
{
    int len, i;
    char *name;

    len = sprintf (buffer, "Serial ports:\n");
    for (i = 0; i < NR_PORTS; ++i) {
	if (!rs_table[i].port) continue;

#ifndef CONFIG_BESTA
	if (rs_table[i].type >= 0 && rs_table[i].type < PORT_MAX)
	    name = serialtypes[rs_table[i].type];
#ifdef __mc68000__
	else if (rs_table[i].type >= 100 &&
		 rs_table[i].type < 100 + M68K_PORT_MAX)
	    name = serialtypes68k[rs_table[i].type - 100];
#endif
	else
	    name = "unknown";

#else   /*  CONFIG_BESTA   */

	/*  There is a lot of serial types possible on the VME board.
	   So, go to machine specific immediately...
	*/
	{
	    extern char *besta_get_serial_type (int);

	    name = besta_get_serial_type (rs_table[i].type);
	}
#endif  /*  CONFIG_BESTA   */

	len += sprintf (buffer + len, "\tttyS%d: %s at 0x%08x.\n", i, name,
			rs_table[i].port);
    }

    if (len > 14)
	return len;

    /* No serial ports detected */
    buffer[0] = 0;
    return 0;
}
#endif
#endif

/*
 * tmp_buf is used as a temporary buffer by serial_write.  We need to
 * lock it in case the memcpy_fromfs blocks while swapping in a page,
 * and some other program tries to do a serial write at the same time.
 * Since the lock will only come under contention when the system is
 * swapping and available memory is low, it makes sense to share one
 * buffer across all the serial ports, since it significantly saves
 * memory if large numbers of serial ports are open.
 */
static unsigned char *tmp_buf = 0;
static struct semaphore tmp_buf_sem = MUTEX;

static inline int serial_paranoia_check(struct async_struct *info,
					kdev_t device, const char *routine)
{
#ifdef SERIAL_PARANOIA_CHECK
	const char *badmagic =
		"Warning: bad magic number for serial struct (%s) in %s\n";
	const char *badinfo =
		"Warning: null async_struct for (%s) in %s\n";

	if (!info) {
		printk(badinfo, kdevname(device), routine);
		return 1;
	}
	if (info->magic != SERIAL_MAGIC) {
		printk(badmagic, kdevname(device), routine);
		return 1;
	}
#endif
	return 0;
}

/*
 * ------------------------------------------------------------
 * rs_stop() and rs_start()
 *
 * This routines are called before setting or resetting tty->stopped.
 * They enable or disable transmitter interrupts, as necessary.
 * ------------------------------------------------------------
 */
void rs_stop(struct tty_struct *tty)
{
	struct async_struct *info = (struct async_struct *)tty->driver_data;

	if (serial_paranoia_check(info, tty->device, "rs_stop"))
		return;

	info->sw->enab_tx_int( info, 0 );
}

void rs_start(struct tty_struct *tty)
{
	struct async_struct *info = (struct async_struct *)tty->driver_data;

	if (serial_paranoia_check(info, tty->device, "rs_start"))
		return;

	info->sw->enab_tx_int( info, 1 );
}

/*
 * This routine is used to handle the "bottom half" processing for the
 * serial driver, known also the "software interrupt" processing.
 * This processing is done at the kernel interrupt level, after the
 * rs_interrupt() has returned, BUT WITH INTERRUPTS TURNED ON.  This
 * is where time-consuming activities which can not be done in the
 * interrupt driver proper are done; the interrupt driver schedules
 * them using rs_sched_event(), and they get done here.
 */
static void do_serial_bh(void)
{
	run_task_queue(&tq_serial);
}

static void do_softint(void *private_)
{
	struct async_struct	*info = (struct async_struct *) private_;
	struct tty_struct	*tty;

	tty = info->tty;
	if (!tty)
		return;

	if (clear_bit(RS_EVENT_WRITE_WAKEUP, &info->event)) {
		if ((tty->flags & (1 << TTY_DO_WRITE_WAKEUP)) &&
		    tty->ldisc.write_wakeup)
			(tty->ldisc.write_wakeup)(tty);
		wake_up_interruptible(&tty->write_wait);
	}
}

/*
 * This routine is called from the scheduler tqueue when the interrupt
 * routine has signalled that a hangup has occurred.  The path of
 * hangup processing is:
 *
 * 	serial interrupt routine -> (scheduler tqueue) ->
 * 	do_serial_hangup() -> tty->hangup() -> rs_hangup()
 *
 */
static void do_serial_hangup(void *private_)
{
	struct async_struct	*info = (struct async_struct *) private_;
	struct tty_struct	*tty;

	tty = info->tty;
	if (!tty)
		return;

	tty_hangup(tty);
}


/*
 * ---------------------------------------------------------------
 * Low level utility subroutines for the serial driver:  routines to
 * figure out the appropriate timeout for an interrupt chain, routines
 * to initialize and startup a serial port, and routines to shutdown a
 * serial port.  Useful stuff like that.
 * ---------------------------------------------------------------
 */

static int startup(struct async_struct * info)
{
	unsigned long flags;
	unsigned long page;

	page = get_free_page(GFP_KERNEL);
	if (!page)
		return -ENOMEM;

	
	save_flags(flags); cli();

	if (info->flags & ASYNC_INITIALIZED) {
		free_page(page);
		restore_flags(flags);
		return 0;
	}

	if (!info->port || !info->type) {
		if (info->tty)
			set_bit(TTY_IO_ERROR, &info->tty->flags);
		free_page(page);
		restore_flags(flags);
		return 0;
	}
	if (info->xmit_buf)
		free_page(page);
	else
		info->xmit_buf = (unsigned char *) page;

#ifdef SERIAL_DEBUG_OPEN
	printk("starting up ttyS%d...", info->line);
#endif

	/* initialize the hardware specific stuff for the port */
	info->sw->init(info);

	if (info->tty)
		clear_bit(TTY_IO_ERROR, &info->tty->flags);
	info->xmit_cnt = info->xmit_head = info->xmit_tail = 0;

	/*
	 * Set the speed and other port parameters.
	 */
	info->sw->change_speed(info);

	info->flags |= ASYNC_INITIALIZED;
	restore_flags(flags);
	return 0;
}

/*
 * This routine will shutdown a serial port; interrupts are disabled, and
 * DTR is dropped if the hangup on close termio flag is on.
 */
static void shutdown(struct async_struct * info)
{
	unsigned long	flags;

	if (!(info->flags & ASYNC_INITIALIZED))
		return;

#ifdef SERIAL_DEBUG_OPEN
	printk("Shutting down serial port %d\n", info->line);
#endif

	save_flags(flags); cli(); /* Disable interrupts */

	/*
	 * clear delta_msr_wait queue to avoid mem leaks: we may free the irq
	 * here so the queue might never be waken up
	 */
	wake_up_interruptible(&info->delta_msr_wait);

	/* do hardware specific deinitialization for the port */
	info->sw->deinit( info, info->tty &&
			 !(info->tty->termios->c_cflag & HUPCL) );

	if (info->xmit_buf) {
		free_page((unsigned long) info->xmit_buf);
		info->xmit_buf = 0;
	}

	if (info->tty)
		set_bit(TTY_IO_ERROR, &info->tty->flags);

	info->flags &= ~ASYNC_INITIALIZED;
	restore_flags(flags);
}

static void rs_put_char(struct tty_struct *tty, unsigned char ch)
{
	struct async_struct *info = (struct async_struct *)tty->driver_data;
	unsigned long flags;

	if (serial_paranoia_check(info, tty->device, "rs_put_char"))
		return;

	if (!tty || !info->xmit_buf)
		return;

	save_flags(flags); cli();

	if (info->xmit_cnt >= SERIAL_XMIT_SIZE - 1) {
		restore_flags(flags);
		return;
	}

	info->xmit_buf[info->xmit_head++] = ch;
	info->xmit_head &= SERIAL_XMIT_SIZE-1;
	info->xmit_cnt++;
	restore_flags(flags);
}

static void rs_flush_chars(struct tty_struct *tty)
{
	struct async_struct *info = (struct async_struct *)tty->driver_data;

	if (serial_paranoia_check(info, tty->device, "rs_flush_chars"))
		return;

	if (info->xmit_cnt <= 0 || tty->stopped || tty->hw_stopped ||
	    !info->xmit_buf)
		return;

	info->sw->enab_tx_int( info, 1 );
}

static int rs_write(struct tty_struct * tty, int from_user,
		    const unsigned char *buf, int count)
{
	int	c, total = 0;
	struct async_struct *info = (struct async_struct *)tty->driver_data;
	unsigned long flags;

	if (serial_paranoia_check(info, tty->device, "rs_write"))
		return 0;

	if (!tty || !info->xmit_buf || !tmp_buf)
		return 0;

	if (from_user)
		down(&tmp_buf_sem);
	save_flags(flags);
	while (1) {
		cli();
		c = MIN(count, MIN(SERIAL_XMIT_SIZE - info->xmit_cnt - 1,
				   SERIAL_XMIT_SIZE - info->xmit_head));
		if (c <= 0)
			break;

		if (from_user) {
			restore_flags(flags);
			memcpy_fromfs(tmp_buf, buf, c);
			cli();
			c = MIN(c, MIN(SERIAL_XMIT_SIZE - info->xmit_cnt - 1,
				       SERIAL_XMIT_SIZE - info->xmit_head));
			memcpy(info->xmit_buf + info->xmit_head, tmp_buf, c);
		} else
			memcpy(info->xmit_buf + info->xmit_head, buf, c);

		info->xmit_head = (info->xmit_head + c) & (SERIAL_XMIT_SIZE-1);
		info->xmit_cnt += c;
		restore_flags(flags);
		buf += c;
		count -= c;
		total += c;
	}
	if (from_user)
		up(&tmp_buf_sem);
	if (info->xmit_cnt && !tty->stopped && !tty->hw_stopped)
		info->sw->enab_tx_int( info, 1 );
	restore_flags(flags);
	return total;
}

static int rs_write_room(struct tty_struct *tty)
{
	struct async_struct *info = (struct async_struct *)tty->driver_data;
	int	ret;

	if (serial_paranoia_check(info, tty->device, "rs_write_room"))
		return 0;
	ret = SERIAL_XMIT_SIZE - info->xmit_cnt - 1;
	if (ret < 0)
		ret = 0;
	return ret;
}

static int rs_chars_in_buffer(struct tty_struct *tty)
{
	struct async_struct *info = (struct async_struct *)tty->driver_data;

	if (serial_paranoia_check(info, tty->device, "rs_chars_in_buffer"))
		return 0;
	return info->xmit_cnt;
}

static void rs_flush_buffer(struct tty_struct *tty)
{
	struct async_struct *info = (struct async_struct *)tty->driver_data;
	unsigned long flags;

	if (serial_paranoia_check(info, tty->device, "rs_flush_buffer"))
		return;
	save_flags(flags);
	cli();
	info->xmit_cnt = info->xmit_head = info->xmit_tail = 0;
	restore_flags(flags);
	wake_up_interruptible(&tty->write_wait);
	if ((tty->flags & (1 << TTY_DO_WRITE_WAKEUP)) &&
	    tty->ldisc.write_wakeup)
		(tty->ldisc.write_wakeup)(tty);
}

/*
 * ------------------------------------------------------------
 * rs_throttle()
 *
 * This routine is called by the upper-layer tty layer to signal that
 * incoming characters should be throttled.
 * ------------------------------------------------------------
 */
static void rs_throttle(struct tty_struct * tty)
{
	struct async_struct *info = (struct async_struct *)tty->driver_data;
#ifdef SERIAL_DEBUG_THROTTLE
	char	buf[64];

	printk("throttle %s: %d....\n", _tty_name(tty, buf),
	       tty->ldisc.chars_in_buffer(tty));
#endif

	if (serial_paranoia_check(info, tty->device, "rs_throttle"))
		return;

	if (I_IXOFF(tty)) {
		info->x_char = STOP_CHAR(tty);
		/* make sure the XOFF char is sent out if the TX was idle */
		info->sw->enab_tx_int( info, 1 );
	}

	if (C_CRTSCTS(tty))
		info->sw->throttle( info, 1 );
}

static void rs_unthrottle(struct tty_struct * tty)
{
	unsigned long flags;
	struct async_struct *info = (struct async_struct *)tty->driver_data;
#ifdef SERIAL_DEBUG_THROTTLE
	char	buf[64];

	printk("unthrottle %s: %d....\n", _tty_name(tty, buf),
	       tty->ldisc.chars_in_buffer(tty));
#endif

	if (serial_paranoia_check(info, tty->device, "rs_unthrottle"))
		return;

	if (I_IXOFF(tty)) {
		/* Protect against an tx int between the test and resetting x_char.
		 * This would cause the XON to be lost. */
		save_flags(flags);
		cli();
		if (info->x_char) {
			info->x_char = 0;
			restore_flags(flags);
		}
		else {
			restore_flags(flags);
			info->x_char = START_CHAR(tty);
			/* make sure the XOFF char is sent out if the TX was idle */
			info->sw->enab_tx_int( info, 1 );
		}
	}

	if (C_CRTSCTS(tty))
		info->sw->throttle( info, 0 );
}

/*
 * ------------------------------------------------------------
 * rs_ioctl() and friends
 * ------------------------------------------------------------
 */

static int get_serial_info(struct async_struct * info,
			   struct serial_struct * retinfo)
{
	struct serial_struct tmp;

	if (!retinfo)
		return -EFAULT;
	memset(&tmp, 0, sizeof(tmp));
	tmp.type = info->type;
	tmp.line = info->line;
	tmp.port = 0;	/* meaningless for non-Intel */
	tmp.irq  = 0;	/* meaningless for non-Intel */
	tmp.flags = info->flags;
	/* tmp.baud_base = info->baud_base;*/
	tmp.close_delay = info->close_delay;
	tmp.closing_wait = info->closing_wait;
	tmp.custom_divisor = info->custom_divisor;
	tmp.hub6 = 0;	/* meaningless for non-Intel */

	/* At least baud_base and costum_divisor set by port-specific
	 * function
	 */
	info->sw->get_serial_info( info, &tmp );

	memcpy_tofs(retinfo,&tmp,sizeof(*retinfo));
	return 0;
}

static int set_serial_info(struct async_struct * info,
			   struct serial_struct * new_info)
{
	struct serial_struct new_serial;
	struct async_struct old_info;
	int retval = 0;

	if (!new_info)
		return -EFAULT;
	memcpy_fromfs(&new_serial,new_info,sizeof(new_serial));
	old_info = *info;

  	/* If a new custom divisor is to be set, let it check by the hardware
 	 * specific code first! (It is given the new info struct in case the
 	 * baud_base has also changed and valid divisors depend on the baud_base.)
	 */
	if (new_serial.custom_divisor != info->custom_divisor ||
 		new_serial.baud_base != info->baud_base) {
		if (info->sw->check_custom_divisor( info, new_serial.baud_base,
						   new_serial.custom_divisor ))
			return( -EINVAL );
	}

	if (!suser()) {
		if (/*(new_serial.baud_base != info->baud_base) ||*/
		    (new_serial.type != info->type) ||
		    (new_serial.close_delay != info->close_delay) ||
		    ((new_serial.flags & ~ASYNC_USR_MASK) !=
		     (info->flags & ~ASYNC_USR_MASK)))
			return -EPERM;
		info->flags = ((info->flags & ~ASYNC_USR_MASK) |
			       (new_serial.flags & ASYNC_USR_MASK));
		info->custom_divisor = new_serial.custom_divisor;
		info->baud_base = new_serial.baud_base;
		goto check_and_exit;
	}

	/*
	 * OK, past this point, all the error checking has been done.
	 * At this point, we start making changes.....
	 */

	info->baud_base = new_serial.baud_base;
	info->flags = ((info->flags & ~ASYNC_FLAGS) |
			(new_serial.flags & ASYNC_FLAGS));
	info->custom_divisor = new_serial.custom_divisor;
	info->type = new_serial.type;
	info->close_delay = new_serial.close_delay * HZ/100;
	info->closing_wait = new_serial.closing_wait * HZ/100;

check_and_exit:
	if (!info->port || !info->type)
		return 0;
	if (info->flags & ASYNC_INITIALIZED) {
		if (((old_info.flags & ASYNC_SPD_MASK) !=
		     (info->flags & ASYNC_SPD_MASK)) ||
		    (old_info.custom_divisor != info->custom_divisor) ||
		    (old_info.baud_base != info->baud_base))
			info->sw->change_speed(info);
	} else
		retval = startup(info);
	return retval;
}


static int get_modem_info(struct async_struct * info, unsigned int *value)
{
	unsigned int result;

	result = info->sw->get_modem_info (info);
	put_user(result,value);
	return 0;
}

static int set_modem_info(struct async_struct * info, unsigned int cmd,
			  unsigned int *value)
{
	int error;
	unsigned int arg;
	int new_dtr = -1, new_rts = -1;

	error = verify_area(VERIFY_READ, value, sizeof(int));
	if (error)
		return error;
	arg = get_user(value);
	switch (cmd) {
	case TIOCMBIS:
		if (arg & TIOCM_RTS)
			new_rts = 1;
		if (arg & TIOCM_DTR)
			new_dtr = 1;
		break;
	case TIOCMBIC:
		if (arg & TIOCM_RTS)
			new_rts = 0;
		if (arg & TIOCM_DTR)
			new_dtr = 0;
		break;
	case TIOCMSET:
		new_dtr = !!(arg & TIOCM_DTR);
		new_rts = !!(arg & TIOCM_RTS);
		break;
	default:
		return -EINVAL;
	}
	return( info->sw->set_modem_info( info, new_dtr, new_rts ) );
}

/*
 * This routine sends a break character out the serial port.
 */
static void send_break(	struct async_struct * info, int duration)
{
	unsigned long flags;

	if (!info->port)
		return;
	current->state = TASK_INTERRUPTIBLE;
	current->timeout = jiffies + duration;
	save_flags(flags);
	cli();
	info->sw->set_break( info, 1 );
	schedule();
	info->sw->set_break( info, 0 );
	restore_flags(flags);
}

static int rs_ioctl(struct tty_struct *tty, struct file * file,
		    unsigned int cmd, unsigned long arg)
{
	int error;
	struct async_struct * info = (struct async_struct *)tty->driver_data;
	int retval;
	struct async_icount cprev, cnow;	/* kernel counter temps */
	struct serial_icounter_struct *p_cuser;	/* user space */
	unsigned long flags;

	if (serial_paranoia_check(info, tty->device, "rs_ioctl"))
		return -ENODEV;

	if ((cmd != TIOCGSERIAL) && (cmd != TIOCSSERIAL) &&
	    (cmd != TIOCSERCONFIG) && (cmd != TIOCSERGWILD)  &&
	    (cmd != TIOCSERSWILD) && (cmd != TIOCSERGSTRUCT) &&
	    (cmd != TIOCMIWAIT) && (cmd != TIOCGICOUNT)) {
		if (tty->flags & (1 << TTY_IO_ERROR))
		    return -EIO;
	}

	switch (cmd) {
		case TCSBRK:	/* SVID version: non-zero arg --> no break */
			retval = tty_check_change(tty);
			if (retval)
				return retval;
			tty_wait_until_sent(tty, 0);
			if (!arg)
				send_break(info, HZ/4);	/* 1/4 second */
			return 0;
		case TCSBRKP:	/* support for POSIX tcsendbreak() */
			retval = tty_check_change(tty);
			if (retval)
				return retval;
			tty_wait_until_sent(tty, 0);
			send_break(info, arg ? arg*(HZ/10) : HZ/4);
			return 0;
		case TIOCGSOFTCAR:
			error = verify_area(VERIFY_WRITE, (void *) arg,sizeof(long));
			if (error)
				return error;
			put_fs_long(C_CLOCAL(tty) ? 1 : 0,
				    (unsigned long *) arg);
			return 0;
		case TIOCSSOFTCAR:
			error = verify_area(VERIFY_READ, (void *) arg,
					    sizeof(unsigned long));
			if (error)
				return error;
			arg = get_fs_long((unsigned long *) arg);
			tty->termios->c_cflag =
				((tty->termios->c_cflag & ~CLOCAL) |
				 (arg ? CLOCAL : 0));
			return 0;
		case TIOCMGET:
			error = verify_area(VERIFY_WRITE, (void *) arg,
				sizeof(unsigned int));
			if (error)
				return error;
			return get_modem_info(info, (unsigned int *) arg);
		case TIOCMBIS:
		case TIOCMBIC:
		case TIOCMSET:
			return set_modem_info(info, cmd, (unsigned int *) arg);
		case TIOCGSERIAL:
			error = verify_area(VERIFY_WRITE, (void *) arg,
						sizeof(struct serial_struct));
			if (error)
				return error;
			return get_serial_info(info,
					       (struct serial_struct *) arg);
		case TIOCSSERIAL:
			error = verify_area(VERIFY_READ, (void *) arg,
					    sizeof(struct serial_struct));
			if (error)
				return error;
			return set_serial_info(info,
					       (struct serial_struct *) arg);
		case TIOCSERCONFIG:
			/* return do_autoconfig(info); */
			return ( 0 );

		case TIOCSERGWILD:
			error = verify_area(VERIFY_WRITE, (void *) arg,
					    sizeof(int));
			if (error)
				return error;
			/* no wild mask */
			put_fs_long(0, (unsigned long *) arg);
			return 0;

		case TIOCSERGETLSR: /* Get line status register */
			error = verify_area(VERIFY_WRITE, (void *) arg,
				sizeof(unsigned int));
			if (error)
				return error;
			else {
				put_fs_long(0,(unsigned long *) arg);
				return 0;
			}

		case TIOCSERSWILD:
			if (!suser())
				return -EPERM;
			return 0;

		case TIOCSERGSTRUCT:
			error = verify_area(VERIFY_WRITE, (void *) arg,
						sizeof(struct async_struct));
			if (error)
				return error;
			memcpy_tofs((struct async_struct *) arg,
				    info, sizeof(struct async_struct));
			return 0;

		case TIOCSERGETMULTI:
		case TIOCSERSETMULTI:
			return -EINVAL;

		/*
		 * Wait for any of the 4 modem inputs (DCD,RI,DSR,CTS) to change
		 * - mask passed in arg for lines of interest
 		 *   (use |'ed TIOCM_RNG/DSR/CD/CTS for masking)
		 * Caller should use TIOCGICOUNT to see which one it was
		 */
		 case TIOCMIWAIT:
			save_flags(flags);
			cli();
			cprev = info->icount;	/* note the counters on entry */
			restore_flags(flags);
			while (1) {
				interruptible_sleep_on(&info->delta_msr_wait);
				/* see if a signal did it */
				if (current->signal & ~current->blocked)
					return -ERESTARTSYS;
				save_flags(flags);
				cli();
				cnow = info->icount;	/* atomic copy */
				restore_flags(flags);
				if (cnow.rng == cprev.rng && cnow.dsr == cprev.dsr && 
				    cnow.dcd == cprev.dcd && cnow.cts == cprev.cts)
					return -EIO; /* no change => error */
				if ( ((arg & TIOCM_RNG) && (cnow.rng != cprev.rng)) ||
				     ((arg & TIOCM_DSR) && (cnow.dsr != cprev.dsr)) ||
				     ((arg & TIOCM_CD)  && (cnow.dcd != cprev.dcd)) ||
				     ((arg & TIOCM_CTS) && (cnow.cts != cprev.cts)) ) {
					return 0;
				}
				cprev = cnow;
			}
			/* NOTREACHED */

		/* 
		 * Get counter of input serial line interrupts (DCD,RI,DSR,CTS)
		 * Return: write counters to the user passed counter struct
		 * NB: both 1->0 and 0->1 transitions are counted except for
		 *     RI where only 0->1 is counted.
		 */
		case TIOCGICOUNT:
			error = verify_area(VERIFY_WRITE, (void *) arg,
				sizeof(struct serial_icounter_struct));
			if (error)
				return error;
			save_flags(flags);
			cli();
			cnow = info->icount;
			restore_flags(flags);
			p_cuser = (struct serial_icounter_struct *) arg;
			put_user(cnow.cts, &p_cuser->cts);
			put_user(cnow.dsr, &p_cuser->dsr);
			put_user(cnow.rng, &p_cuser->rng);
			put_user(cnow.dcd, &p_cuser->dcd);
			return 0;

		default:
			if (info->sw->ioctl)
				return( info->sw->ioctl( tty, file, info, cmd, arg ) );
			else
				return -ENOIOCTLCMD;
		}
	return 0;
}

static void rs_set_termios(struct tty_struct *tty, struct termios *old_termios)
{
	struct async_struct *info = (struct async_struct *)tty->driver_data;

#ifndef CONFIG_BESTA
	if (tty->termios->c_cflag == old_termios->c_cflag)
		return;
#else
	if (tty->termios->c_cflag == old_termios->c_cflag &&
	    tty->termios->c_iflag == old_termios->c_iflag)
		return;
#endif

	info->sw->change_speed(info);

	if ((old_termios->c_cflag & CRTSCTS) &&
	    !(tty->termios->c_cflag & CRTSCTS)) {
		tty->hw_stopped = 0;
		rs_start(tty);
	}

#if 0
	/*
	 * No need to wake up processes in open wait, since they
	 * sample the CLOCAL flag once, and don't recheck it.
	 * XXX  It's not clear whether the current behavior is correct
	 * or not.  Hence, this may change.....
	 */
	if (!(old_termios->c_cflag & CLOCAL) &&
	    (tty->termios->c_cflag & CLOCAL))
		wake_up_interruptible(&info->open_wait);
#endif
}

/*
 * ------------------------------------------------------------
 * rs_close()
 *
 * This routine is called when the serial port gets closed.  First, we
 * wait for the last remaining data to be sent.  Then, we unlink its
 * async structure from the interrupt chain if necessary, and we free
 * that IRQ if nothing is left in the chain.
 * ------------------------------------------------------------
 */
static void rs_close(struct tty_struct *tty, struct file * filp)
{
	struct async_struct * info = (struct async_struct *)tty->driver_data;
	unsigned long flags;
	unsigned long timeout;

	if (!info || serial_paranoia_check(info, tty->device, "rs_close"))
		return;

	save_flags(flags); cli();

	if (tty_hung_up_p(filp)) {
		MOD_DEC_USE_COUNT;
		restore_flags(flags);
		return;
	}

#ifdef SERIAL_DEBUG_OPEN
	printk("rs_close ttys%d, count = %d\n", info->line, info->count);
#endif
	if ((tty->count == 1) && (info->count != 1)) {
		/*
		 * Uh, oh.  tty->count is 1, which means that the tty
		 * structure will be freed.  Info->count should always
		 * be one in these conditions.  If it's greater than
		 * one, we've got real problems, since it means the
		 * serial port won't be shutdown.
		 */
		printk("rs_close: bad serial port count; tty->count is 1, "
		       "info->count is %d\n", info->count);
		info->count = 1;
	}
	if (--info->count < 0) {
		printk("rs_close: bad serial port count for ttys%d: %d\n",
		       info->line, info->count);
		info->count = 0;
	}
	if (info->count) {
		MOD_DEC_USE_COUNT;
		restore_flags(flags);
		return;
	}
	info->flags |= ASYNC_CLOSING;
	/*
	 * Save the termios structure, since this port may have
	 * separate termios for callout and dialin.
	 */
	if (info->flags & ASYNC_NORMAL_ACTIVE)
		info->normal_termios = *tty->termios;
	if (info->flags & ASYNC_CALLOUT_ACTIVE)
		info->callout_termios = *tty->termios;
	/*
	 * Now we wait for the transmit buffer to clear; and we notify
	 * the line discipline to only process XON/XOFF characters.
	 */
	tty->closing = 1;
	if (info->closing_wait != ASYNC_CLOSING_WAIT_NONE)
		tty_wait_until_sent(tty, info->closing_wait);
	/*
	 * At this point we stop accepting input.  To do this, we
	 * disable the receive line status interrupts, and tell the
	 * interrupt driver to stop checking the data ready bit in the
	 * line status register.
	 */
	if (info->flags & ASYNC_INITIALIZED) {
		info->sw->stop_receive(info);
		/*
		 * Before we drop DTR, make sure the UART transmitter
		 * has completely drained; this is especially
		 * important if there is a transmit FIFO!
		 */
		timeout = jiffies+HZ;
		while (!(info->sw->trans_empty(info))) {
			current->state = TASK_INTERRUPTIBLE;
			current->timeout = jiffies + info->timeout;
			schedule();
			if (jiffies > timeout)
				break;
		}
	}
	shutdown(info);
	if (tty->driver.flush_buffer)
		tty->driver.flush_buffer(tty);
	if (tty->ldisc.flush_buffer)
		tty->ldisc.flush_buffer(tty);
	tty->closing = 0;
	info->event = 0;
	info->tty = 0;
	if (info->blocked_open) {
		if (info->close_delay) {
			current->state = TASK_INTERRUPTIBLE;
			current->timeout = jiffies + info->close_delay;
			schedule();
		}
		wake_up_interruptible(&info->open_wait);
	}
	info->flags &= ~(ASYNC_NORMAL_ACTIVE|ASYNC_CALLOUT_ACTIVE|
			 ASYNC_CLOSING);
	wake_up_interruptible(&info->close_wait);
	MOD_DEC_USE_COUNT;
	restore_flags(flags);
}

/*
 * rs_hangup() --- called by tty_hangup() when a hangup is signaled.
 */
void rs_hangup(struct tty_struct *tty)
{
	struct async_struct * info = (struct async_struct *)tty->driver_data;

	if (serial_paranoia_check(info, tty->device, "rs_hangup"))
		return;

	rs_flush_buffer(tty);
	shutdown(info);
	info->event = 0;
	info->count = 0;
	info->flags &= ~(ASYNC_NORMAL_ACTIVE|ASYNC_CALLOUT_ACTIVE);
	info->tty = 0;
	wake_up_interruptible(&info->open_wait);
}

/*
 * ------------------------------------------------------------
 * rs_open() and friends
 * ------------------------------------------------------------
 */
static int block_til_ready(struct tty_struct *tty, struct file * filp,
			   struct async_struct *info)
{
	struct wait_queue wait = { current, NULL };
	int		retval;
	int		do_clocal = 0;
	unsigned long flags;

	/*
	 * If the device is in the middle of being closed, then block
	 * until it's done, and then try again.
	 */
	if (tty_hung_up_p(filp) ||
	    (info->flags & ASYNC_CLOSING)) {
		if (info->flags & ASYNC_CLOSING)
			interruptible_sleep_on(&info->close_wait);
#ifdef SERIAL_DO_RESTART
		if (info->flags & ASYNC_HUP_NOTIFY)
			return -EAGAIN;
		else
			return -ERESTARTSYS;
#else
		return -EAGAIN;
#endif
	}

	/*
	 * If this is a callout device, then just make sure the normal
	 * device isn't being used.
	 */
	if (tty->driver.subtype == SERIAL_TYPE_CALLOUT) {
		if (info->flags & ASYNC_NORMAL_ACTIVE)
			return -EBUSY;
		if ((info->flags & ASYNC_CALLOUT_ACTIVE) &&
		    (info->flags & ASYNC_SESSION_LOCKOUT) &&
		    (info->session != current->session))
		    return -EBUSY;
		if ((info->flags & ASYNC_CALLOUT_ACTIVE) &&
		    (info->flags & ASYNC_PGRP_LOCKOUT) &&
		    (info->pgrp != current->pgrp))
		    return -EBUSY;
		info->flags |= ASYNC_CALLOUT_ACTIVE;
		return 0;
	}

	/*
	 * If non-blocking mode is set, or the port is not enabled,
	 * then make the check up front and then exit.
	 */
	if ((filp->f_flags & O_NONBLOCK) ||
	    (tty->flags & (1 << TTY_IO_ERROR))) {
		if (info->flags & ASYNC_CALLOUT_ACTIVE)
			return -EBUSY;
		info->flags |= ASYNC_NORMAL_ACTIVE;
		return 0;
	}

	if (info->flags & ASYNC_CALLOUT_ACTIVE) {
		if (info->normal_termios.c_cflag & CLOCAL)
			do_clocal = 1;
	} else {
		if (tty->termios->c_cflag & CLOCAL)
			do_clocal = 1;
	}

	/*
	 * Block waiting for the carrier detect and the line to become
	 * free (i.e., not in use by the callout).  While we are in
	 * this loop, info->count is dropped by one, so that
	 * rs_close() knows when to free things.  We restore it upon
	 * exit, either normal or abnormal.
	 */
	retval = 0;
	add_wait_queue(&info->open_wait, &wait);
#ifdef SERIAL_DEBUG_OPEN
	printk("block_til_ready before block: ttys%d, count = %d\n",
	       info->line, info->count);
#endif
	save_flags(flags);
	cli();
	if (!tty_hung_up_p(filp))
		info->count--;
	restore_flags(flags);
	info->blocked_open++;
	while (1) {
		save_flags(flags);
		cli();
		if (!(info->flags & ASYNC_CALLOUT_ACTIVE))
			info->sw->set_modem_info( info, 1, 1 );
		restore_flags(flags);
		current->state = TASK_INTERRUPTIBLE;
		if (tty_hung_up_p(filp) ||
		    !(info->flags & ASYNC_INITIALIZED)) {
#ifdef SERIAL_DO_RESTART
			if (info->flags & ASYNC_HUP_NOTIFY)
				retval = -EAGAIN;
			else
				retval = -ERESTARTSYS;
#else
			retval = -EAGAIN;
#endif
			break;
		}
		if (!(info->flags & ASYNC_CALLOUT_ACTIVE) &&
		    !(info->flags & ASYNC_CLOSING) &&
		    (do_clocal || (info->sw->get_modem_info( info ) & TIOCM_CAR)))
			break;
		if (current->signal & ~current->blocked) {
			retval = -ERESTARTSYS;
			break;
		}
#ifdef SERIAL_DEBUG_OPEN
		printk("block_til_ready blocking: ttys%d, count = %d\n",
		       info->line, info->count);
#endif
		schedule();
	}
	current->state = TASK_RUNNING;
	remove_wait_queue(&info->open_wait, &wait);
	if (!tty_hung_up_p(filp))
		info->count++;
	info->blocked_open--;
#ifdef SERIAL_DEBUG_OPEN
	printk("block_til_ready after blocking: ttys%d, count = %d\n",
	       info->line, info->count);
#endif
	if (retval)
		return retval;
	info->flags |= ASYNC_NORMAL_ACTIVE;
	return 0;
}

/*
 * This routine is called whenever a serial port is opened.  It
 * enables interrupts for a serial port, linking in its async structure into
 * the IRQ chain.   It also performs the serial-specific
 * initialization for the tty structure.
 */
int rs_open(struct tty_struct *tty, struct file * filp)
{
	struct async_struct	*info;
	int 			retval, line;
	unsigned long		page;

	line = MINOR(tty->device) - tty->driver.minor_start;
	if ((line < 0) || (line >= NR_PORTS))
		return -ENXIO;
	info = rs_table + line;
#ifdef CONFIG_KERNELD
	if (!info->port) {
		char modname[30];
		sprintf(modname, "char-major-%d-%d", TTY_MAJOR, MINOR(tty->device));
		request_module(modname);
	}
#endif
	if (!info->port)
		return -ENODEV;
	if (serial_paranoia_check(info, tty->device, "rs_open"))
		return -ENODEV;

	if (info->sw->check_open) {
		if ((retval = info->sw->check_open( info, tty, filp )))
			return( retval );
	}

#ifdef SERIAL_DEBUG_OPEN
	printk("rs_open %s%d, count = %d\n", tty->driver.name, info->line,
	       info->count);
#endif
	info->count++;
	tty->driver_data = info;
	info->tty = tty;

	if (!tmp_buf) {
		page = get_free_page(GFP_KERNEL);
		if (!page)
			return -ENOMEM;
		if (tmp_buf)
			free_page(page);
		else
			tmp_buf = (unsigned char *) page;
	}

	/*
	 * Start up serial port
	 */
	retval = startup(info);
	if (retval)
		return retval;

	MOD_INC_USE_COUNT;
	retval = block_til_ready(tty, filp, info);
	if (retval) {
#ifdef SERIAL_DEBUG_OPEN
		printk("rs_open returning after block_til_ready with %d\n",
		       retval);
#endif
		return retval;
	}

	if ((info->count == 1) && (info->flags & ASYNC_SPLIT_TERMIOS)) {
		if (tty->driver.subtype == SERIAL_TYPE_NORMAL)
			*tty->termios = info->normal_termios;
		else
			*tty->termios = info->callout_termios;
		info->sw->change_speed(info);
	}

	info->session = current->session;
	info->pgrp = current->pgrp;

#ifdef SERIAL_DEBUG_OPEN
	printk("rs_open ttys%d successful...", info->line);
#endif
	return 0;
}

/*
 * ---------------------------------------------------------------------
 * rs_init() and friends
 *
 * rs_init() is called at boot-time to initialize the serial driver.
 * ---------------------------------------------------------------------
 */

/*
 * Boot-time initialization code: Init the serial ports present on
 * this machine type.
 *
 * Current HW Port to minor/device name mapping:
 *
 * Atari
 *   TT                    | Falcon                    || minor | name
 *  -----------------------+---------------------------++-------+-------
 *   ST-MFP port (Modem1)  | (Modem1, internal only)   || 64    | ttyS0
 *   SCC B (Modem2)        | SCC B (Modem2)            || 65    | ttyS1
 *   TT-MFP port (Serial1) |                           || 66    | ttyS2
 *   SCC A (Serial2)       |                           || 67    | ttyS3
 *   SCC A (LAN)           | SCC A (LAN)               || 68    | ttyS4
 *   Atari MIDI            | Atari MIDI                || 69    | ttyS5
 *
 * New 22/12/94: Midi is implemented as a serial device to make use of
 * MIDI networks easier (SLIP on ttyS5). If there is need for MIDI
 * specific high-level code, it can be implemented as a line
 * discipline in future. ttyS4 is reserved to make space for the LAN
 * device. It seems to be the better solution to split Serial2 and LAN
 * into separate minors. And maybe the port to device mapping will
 * change in future...
 *
 * 02/08/95: Now the mapping has changed! My old mapping scheme of the serial
 * ports was a flaw in respect to order the devices following their
 * capabilities. And it confused the users, to... The "new" order is the same
 * as the one used by TOS. The only difference is that Serial2/LAN is split
 * into two devices, only one of which can be open at one time. If one of
 * these devices is opened, the respective port's hardware is activated via
 * IO7 of the PSG (if connected...)
 *
 * Amiga
 *   Amiga built-in serial port                        || 64    | ttyS0
 *
 * New 13/07/95: Support for multiple serial-boards of the same kind,
 * and boards with dual (or higher) uarts. This is done by letting the
 * drivers 'allocate' tty's in the rs_table[rs_count] array. A counter
 * (rs_count) is therefore passed to all serial-init routines, that
 * adds the amount of uarts detected, and returns the new value.
 * I think this is the best way to manage the various serial-boards
 * available (atleast for the Amiga), and should cause no harm to the
 * old drivers.                         - Jes Sorensen (jds@kom.auc.dk)
 *
 */

/*
 * This routine prints out the appropriate serial driver version
 * number, and identifies which options were configured into this
 * driver.
 */
static void show_serial_version(void)
{
	printk(KERN_INFO "M68K Serial driver version 1.01\n");
}

static struct symbol_table serial_syms = {
#include <linux/symtab_begin.h>
	X(rs_table),
	X(tq_serial),
	X(register_serial),
	X(unregister_serial),
	X(rs_start),
	X(rs_stop),
	X(rs_init),
#include <linux/symtab_end.h>
};

/*
 * The serial driver boot-time initialization code!
 */
int rs_init(void)
{
	int i, rs_count;
	struct async_struct * info;

	rs_count = 0; /* All machines start at rs_table[0] (minor 64). */

	init_bh(SERIAL_BH, do_serial_bh);

	show_serial_version();

	/* Initialize the tty_driver structure */

	memset(&serial_driver, 0, sizeof(struct tty_driver));
	serial_driver.magic = TTY_DRIVER_MAGIC;
	serial_driver.name = "ttyS";
	serial_driver.major = TTY_MAJOR;
	serial_driver.minor_start = 64;
	serial_driver.num = NR_PORTS;
	serial_driver.type = TTY_DRIVER_TYPE_SERIAL;
	serial_driver.subtype = SERIAL_TYPE_NORMAL;
	serial_driver.init_termios = tty_std_termios;
	serial_driver.init_termios.c_cflag =
		B9600 | CS8 | CREAD | HUPCL | CLOCAL;
	serial_driver.flags = TTY_DRIVER_REAL_RAW;
	serial_driver.refcount = &serial_refcount;
	serial_driver.table = serial_table;
	serial_driver.termios = serial_termios;
	serial_driver.termios_locked = serial_termios_locked;

	serial_driver.open = rs_open;
	serial_driver.close = rs_close;
	serial_driver.write = rs_write;
	serial_driver.put_char = rs_put_char;
	serial_driver.flush_chars = rs_flush_chars;
	serial_driver.write_room = rs_write_room;
	serial_driver.chars_in_buffer = rs_chars_in_buffer;
	serial_driver.flush_buffer = rs_flush_buffer;
	serial_driver.ioctl = rs_ioctl;
	serial_driver.throttle = rs_throttle;
	serial_driver.unthrottle = rs_unthrottle;
	serial_driver.set_termios = rs_set_termios;
	serial_driver.stop = rs_stop;
	serial_driver.start = rs_start;
	serial_driver.hangup = rs_hangup;

	/*
	 * The callout device is just like normal device except for
	 * major number and the subtype code.
	 */
	callout_driver = serial_driver;
	callout_driver.name = "cua";
	callout_driver.major = TTYAUX_MAJOR;
	callout_driver.subtype = SERIAL_TYPE_CALLOUT;

	if (tty_register_driver(&serial_driver)) {
		printk("Couldn't register serial driver\n");
		return -EIO;
	}
	if (tty_register_driver(&callout_driver)) {
		tty_unregister_driver(&serial_driver);
		printk("Couldn't register callout driver\n");
		return -EIO;
	}

	if (register_symtab(&serial_syms) != 0) {
		tty_unregister_driver(&serial_driver);
		tty_unregister_driver(&callout_driver);
		printk("Couldn't register serial symtab\n");
		return -EIO;
	}

	for (i = 0, info = rs_table; i < NR_PORTS; i++,info++) {
		info->magic = SERIAL_MAGIC;
		info->line = i;
		info->port = 0;
		info->tty = 0;
		info->type = PORT_UNKNOWN;
		info->custom_divisor = 0;
		info->close_delay = 5*HZ/10;
		info->closing_wait = 30*HZ;
		info->x_char = 0;
		info->event = 0;
		info->count = 0;
		info->blocked_open = 0;
		info->tqueue.routine = do_softint;
		info->tqueue.data = info;
		info->tqueue_hangup.routine = do_serial_hangup;
		info->tqueue_hangup.data = info;
		info->callout_termios =callout_driver.init_termios;
		info->normal_termios = serial_driver.init_termios;
		info->open_wait = 0;
		info->close_wait = 0;
		info->delta_msr_wait = 0;
		info->icount.cts = info->icount.dsr = 0;
		info->icount.rng = info->icount.dcd = 0;
		info->next_port = 0;
		info->prev_port = 0;
	}

#ifndef MODULE
#ifdef CONFIG_PROC_FS
	proc_register_dynamic(&proc_root, &(struct proc_dir_entry) {
		0, 6, "serial",
		S_IFREG | S_IRUGO, 1, 0, 0,
		0, NULL /* ops -- default to array */,
		&proc_serial_read /* get_info */,
	});
#endif /* PROC_FS */
	switch (boot_info.machtype) {
	    case MACH_ATARI:
#ifdef CONFIG_ATARI_SCC
	    atari_SCC_init();
#endif
#ifdef CONFIG_ATARI_MFPSER
	    atari_MFPser_init();
#endif
#ifdef CONFIG_ATARI_MIDI
	    atari_MIDI_init();
#endif
	    break;

	    case MACH_AMIGA:
#ifdef CONFIG_AMIGA_BUILTIN_SERIAL
	    amiga_serinit();
#endif
#ifdef CONFIG_GVPIOEXT
	    ioext_init();
#endif
#ifdef CONFIG_MULTIFACE_III_TTY
	    multiface_init();
#endif
#ifdef CONFIG_WHIPPET
	    whippet_init();
#endif
#ifdef CONFIG_ASDG_DSB
	asdg_setup();
#endif
	    break;
	} /* end switch on machine type */
#endif

	return 0;
}

/*
 * register_serial and unregister_serial allows for serial ports to be
 * configured at run-time, to support PCMCIA modems.
 *
 * ++roman: Removed PCish stuff from here... New calling interface: The caller
 * can set req->line to a value >= 0 to request a particular slot. If
 * req->line is < 0, the first free slot is assigned. Other fields that must
 * be filled in before the call are 'port' and 'type'.
 */
int register_serial(struct serial_struct *req)
{
	int i;
	unsigned long flags;
	struct async_struct *info;

	save_flags(flags);
	cli();
#ifndef __mc68000__
	for (i = 0; i < NR_PORTS; i++) {
		if (rs_table[i].port == req->port)
			break;
	}
	if (i == NR_PORTS) {
#else
	if (req->line >= 0) {
		if (req->line >= NR_PORTS)
		    return -EINVAL;
		if (rs_table[req->line].type != PORT_UNKNOWN ||
		    rs_table[req->line].count > 0) {
		    /* already allocated */
		    restore_flags(flags);
		    return -EBUSY;
		}
		i = req->line;
	}
	else {
#endif
		for (i = 0; i < NR_PORTS; i++)
			if ((rs_table[i].type == PORT_UNKNOWN) &&
			    (rs_table[i].count == 0))
				break;
	}
	if (i == NR_PORTS) {
		restore_flags(flags);
		return -EAGAIN;
	}
	info = &rs_table[i];
	if (rs_table[i].count) {
		restore_flags(flags);
		printk("Couldn't configure serial #%d (port=%d,irq=%d): "
		       "device already open\n", i, req->port, req->irq);
		return -EBUSY;
	}
#ifndef __mc68000__
	info->irq = req->irq;
	info->port = req->port;
	autoconfig(info);
	if (info->type == PORT_UNKNOWN) {
		restore_flags(flags);
		printk("register_serial(): autoconfig failed\n");
		return -1;
	}
	printk(KERN_INFO "tty%02d at 0x%04x (irq = %d)", info->line,
	       info->port, info->irq);
#else
	info->port = req->port;
	info->type = req->type;
	printk(KERN_INFO "ttyS%d at 0x%08x: ", info->line, info->port);
#endif
	if (info->type >= 0 && info->type < PORT_MAX)
	    printk( "%s\n", serialtypes[info->type] );
#ifdef __mc68000__
	else if (info->type >= 100 && info->type < 100 + M68K_PORT_MAX)
	    printk( "%s\n", serialtypes68k[info->type - 100] );
#endif
	else
	    printk( "\n" );
	restore_flags(flags);
	return info->line;
}

void unregister_serial(int line)
{
	unsigned long flags;
	struct async_struct *info = &rs_table[line];

	save_flags(flags);
	cli();
	if (info->tty)
		tty_hangup(info->tty);
	info->type = PORT_UNKNOWN;
#ifdef __mc68000__
	info->port = 0;
#endif
	printk(KERN_INFO "tty%02d unloaded\n", info->line);
	restore_flags(flags);
}

#ifdef MODULE
int init_module(void)
{
	return rs_init();
}

void cleanup_module(void)
{
	if (tty_unregister_driver(&serial_driver))
		printk("SERIAL: failed to unregister serial driver\n");
	if (tty_unregister_driver(&callout_driver))
		printk("SERIAL: failed to unregister callout driver\n");
}
#endif /* MODULE */
