/*
 * ser_whippet.c
 *
 * Copyright (C) 1997 Chris Sumner (chris@ganymede.sonnet.co.uk)
 *
 * This is a driver for the Hisoft Whippet PCMCIA serial port for
 * the Amiga. (16c550b UART)
 *
 * The code is mostly based on ser_ioext.c by Jes Sorensen,
 * (jds@kom.auc.dk) but has been modified to cope with the different
 * hardware footprint of the Whippet. So there.
 *
 */

#include <linux/types.h>
#include <linux/sched.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/malloc.h>
#include <linux/termios.h>
#include <linux/tty.h>
#include <linux/serial.h>

#include <asm/io.h>
#include <asm/setup.h>
#include <asm/irq.h>
#include <asm/amigahw.h>
#include <asm/amigaints.h>
#include <asm/zorro.h>
#include <asm/amigatypes.h>

#include "ser_whippet.h"

#define WHIPPET_DEBUG	0	/* Remove this when the driver is finished */
#undef	WHIPPET_DEBUG

#define FIFO_SIZE 16		/* Size of hardware send and receive FIFOs */

#define WHIPPET_FAST_RATES 1	/*
				 * When this is set, the rates 110 and
				 * 134 will run 57600 and 115200
				 * respectively.
				 */

#define FIFO_TRIGGER_LEVEL	FIFO_TRIG_8
				/*
				 * There are 4 receiver FIFO-interrupt *
				 * trigger levels (FIFO_TRIG_x), that  *
				 * indicates how many bytes are to be  *
				 * allowed in the receiver-FIFO before *
				 * an interrupt is generated:          *
				 *                x =  1 =  1 byte     *
				 *                x =  4 =  4 bytes    *
				 *                x =  8 =  8 bytes    *
				 *                x = 14 = 14 bytes    *
				 * 14 works for me, but if you keep    *
				 * getting overruns try lowering this  *
				 * value one step at a time.           *
				 */

/***************************** Prototypes *****************************/
static void ser_init(struct async_struct *info);

static void ser_deinit(struct async_struct *info, int leave_dtr);

static void ser_enab_tx_int(struct async_struct *info, int enab_flag);

static int  ser_check_custom_divisor(struct async_struct *info,
				     int baud_base, int divisor);

static void ser_change_speed(struct async_struct *info);

static void ser_throttle(struct async_struct *info, int status);

static void ser_set_break(struct async_struct *info, int break_flag);

static void ser_get_serial_info(struct async_struct *info,
				struct serial_struct *retinfo);

static unsigned int ser_get_modem_info(struct async_struct *info);

static int ser_set_modem_info(struct async_struct *info, int new_dtr,
			      int new_rts);

static void ser_stop_receive(struct async_struct *info);

static int ser_trans_empty(struct async_struct *info);


/************************* End of Prototypes **************************/

/*
 * SERIALSWITCH structure for the Whippet serial interface.
 */

static SERIALSWITCH whippet_ser_switch = {
	ser_init,
	ser_deinit,
	ser_enab_tx_int,
	ser_check_custom_divisor,
	ser_change_speed,
	ser_throttle,
	ser_set_break,
	ser_get_serial_info,
	ser_get_modem_info,
	ser_set_modem_info,
	NULL,
	ser_stop_receive, ser_trans_empty, NULL
};

static int whippet_baud_table[18] = {
	/* B0     */ 0,		/* Never use this value !!! */
	/* B50    */ 9216,
	/* B75    */ 6144,
	/* B110   */ 4189,	/* There's a little rounding on this one */
	/* B134   */ 3439,	/* Same here! */
	/* B150   */ 3072,
	/* B200   */ 2304,

#if WHIPPET_FAST_RATES
	/* B57600 */ 8,
#else
	/* B300   */ 1536,
#endif
	/* B600   */ 768,
#if WHIPPET_FAST_RATES
	/* B115k2 */ 4,
#else
	/* B1200  */ 384,
#endif
	/* B1800  */ 256,
	/* B2400  */ 192,
	/* B4800  */ 96,
	/* B9600  */ 48,
	/* B19200 */ 24,
	/* B38400 */ 12,	/* The last of the standard rates.  */
	/* B57600 */ 8,		/* ASYNC_SPD_HI                     */
	/* B115K2 */ 4		/* ASYNC_SPD_VHI                    */
};

static int line; 	/* The line assigned to us by register_serial() */


/***** start of ser_interrupt() - Handler for serial interrupt. *****/

static void ser_interrupt(int irq, void *data, struct pt_regs *regs)
{
struct async_struct *info = data;
u_char iir,lsr;
unsigned char ch;

/* Get value in IIR for future use */

	if ((iir=uart.IIR) & IRQ_PEND)	return;


/* If we got here, then there is an interrupt waiting for us to service */

	while (!(iir & IRQ_PEND))	/* loop until no more ints */
	{
		switch (iir & (IRQ_ID1 | IRQ_ID2 | IRQ_ID3))
		{
			case IRQ_RLS:		/* Receiver Line Status */
			case IRQ_CTI:		/* Character Timeout */
			case IRQ_RDA:		/* Received Data Available */
	/*
	 * Copy chars to the tty-queue ...
	 * Be careful that we aren't passing one of the
	 * Receiver Line Status interrupt-conditions without noticing.
	 */
			{
				int ch;

				lsr = uart.LSR;
				while (lsr & DR)
				{
					u_char err = 0;
					ch = uart.RBR;

					if (lsr & BI)      err = TTY_BREAK;
					else if (lsr & PE) err = TTY_PARITY;
					else if (lsr & OE) err = TTY_OVERRUN;
					else if (lsr & FE) err = TTY_FRAME;

					rs_receive_char(info, ch, err);
					lsr = uart.LSR;
				}
			}
			break;

			case IRQ_THRE:	/* Transmitter holding register empty */
			{
				int fifo_space = 16;

	/* If the uart is ready to receive data and there are chars in */
	/* the queue we transfer all we can to the uart's FIFO         */

				if (info->xmit_cnt <= 0    ||
				    info->tty->stopped     ||
				    info->tty->hw_stopped)
				{

		/* Disable transmitter empty interrupt */
					uart.IER &= ~(ETHREI);

		/* Need to send a char to acknowledge the interrupt */
					uart.THR = 0;
					break;
				}

		/* Handle software flow control */
				if (info->x_char)
				{
					uart.THR = info->x_char;
					info->x_char = 0;
					fifo_space--;
				}

		/* Fill the fifo */
				while (fifo_space > 0)
				{
					fifo_space--;
					uart.THR = info->xmit_buf[info->xmit_tail++];
					info->xmit_tail = info->xmit_tail & (SERIAL_XMIT_SIZE-1);
					if (--info->xmit_cnt == 0) break;
				}

		/* Don't need THR interrupts any more */
				if (info->xmit_cnt == 0)
					uart.IER &= ~(ETHREI);

				if (info->xmit_cnt < WAKEUP_CHARS)
				    rs_sched_event(info, RS_EVENT_WRITE_WAKEUP);
			}
			break;

			case IRQ_MS: /* Must be modem status register interrupt? */
			{
				u_char msr = uart.MSR;

				if (info->flags & ASYNC_INITIALIZED)
				{
					if (msr & DCTS)
						rs_check_cts(info, (msr & CTS));	/* active high */
					if (msr & DDCD)
						rs_dcd_changed(info, !(msr & DCD));	/* active low */
				}
			}
			break;

		} /* switch (iir) */
		iir = uart.IIR;
	} /* while IRQ_PEND */

	ch=inb(GAYLE_IRQ_STATUS);
/*	printk("gayle: %02X - %02X (pre)\n",inb(GAYLE_IRQ_STATUS2),inb(GAYLE_IRQ_STATUS)); */
	outb(0xdc | (ch & 0x03),GAYLE_IRQ_STATUS);
/*	printk("gayle: %02X - %02X (post)\n",inb(GAYLE_IRQ_STATUS2), inb(GAYLE_IRQ_STATUS)); */
}

/***** end of ser_interrupt() *****/


/***** start of ser_init() *****/

static void ser_init(struct async_struct *info)
{
unsigned char ch;

	ch=inb(GAYLE_IRQ_STATUS);
	outb(0xdc | (ch & 0x03),GAYLE_IRQ_STATUS);

	while ((uart.LSR) & DR)
		(void)uart.RBR;		/* read a byte */

/* Set DTR and RTS */
	uart.MCR |= (DTR | RTS);

/* Enable interrupts. IF_EXTER irq has already been enabled in whippet_init()*/
/* DON'T enable ETHREI here because there is nothing to send yet (murray) */
	uart.IER |= (ERDAI | ELSI | EMSI);
}

/***** end of ser_init() *****/


/***** start of ser_deinit() *****/

static void ser_deinit(struct async_struct *info, int leave_dtr)
{
unsigned char ch;

	/* Wait for the uart to get empty */
	while(!((uart.LSR) & TEMT))
	{
	}

	while((uart.LSR) & DR)
	{
		(void)uart.RBR;
	}

	ch=inb(GAYLE_IRQ_STATUS);
	outb(0xdc | (ch & 0x03),GAYLE_IRQ_STATUS);

/* No need to disable UART interrupts since this will already
 * have been done via ser_enab_tx_int() and ser_stop_receive()
 */

	ser_RTSoff;
	if (!leave_dtr)		ser_DTRoff;
}

/***** end of ser_deinit() *****/


/***** start of ser_enab_tx_int() *****/

/*
** Enable or disable tx interrupts.
** Note that contrary to popular belief, it is not necessary to
** send a character to cause an interrupt to occur. Whenever the
** THR is empty and THRE interrupts are enabled, an interrupt will occur.
** (murray)
*/
static void ser_enab_tx_int(struct async_struct *info, int enab_flag)
{
	if (enab_flag)
		uart.IER |= ETHREI;
	else
		uart.IER &= ~(ETHREI);
}

/***** end of ser_enab_tx_int() *****/


static int  ser_check_custom_divisor(struct async_struct *info,
				     int baud_base, int divisor)
{
	/* Always return 0 or else setserial spd_hi/spd_vhi doesn't work */
	return 0;
}


/***** start of ser_change_speed() *****/

static void ser_change_speed(struct async_struct *info)
{
u_int cflag, baud, chsize, stopb, parity, aflags;
u_int div = 0, ctrl = 0;

	if (!info->tty || !info->tty->termios) return;

	cflag  = info->tty->termios->c_cflag;
	baud   = cflag & CBAUD;
	chsize = cflag & CSIZE;
	stopb  = cflag & CSTOPB;
	parity = cflag & (PARENB | PARODD);
	aflags = info->flags & ASYNC_SPD_MASK;

	if (cflag & CRTSCTS)	info->flags |= ASYNC_CTS_FLOW;
	else			info->flags &= ~ASYNC_CTS_FLOW;
	if (cflag & CLOCAL)	info->flags &= ~ASYNC_CHECK_CD;
	else			info->flags |= ASYNC_CHECK_CD;

	if (baud & CBAUDEX)
	{
		baud &= ~CBAUDEX;
		if (baud < 1 || baud > 2)
			info->tty->termios->c_cflag &= ~CBAUDEX;
		else
			baud += 15;
	}
	if (baud == 15)
	{
		if (aflags == ASYNC_SPD_HI)	/*  57k6 */
			baud += 1;
		if (aflags == ASYNC_SPD_VHI)	/* 115k2 */
			baud += 2;
		if (aflags == ASYNC_SPD_CUST)
			div = info->custom_divisor;
	}
	if (!div)
	{
		/* Maximum speed is 115200 */
			if (baud > 17) baud = 17;
			div = whippet_baud_table[baud];
	}

	if (!div)
	{
		/* speed == 0 -> drop DTR */
		ser_DTRoff;
		return;
	}

/*
 * We have to set DTR when a valid rate is chosen, otherwise DTR
 * might get lost when programs use this sequence to clear the line:
 *
 * change_speed(baud = B0);
 * sleep(1);
 * change_speed(baud = Bx); x != 0
 *
 * The pc-guys do this aswell.
 */
	ser_DTRon;

	if (chsize == CS8)
		ctrl |= data_8bit;
	else if (chsize == CS7)
		ctrl |= data_7bit;
	else if	(chsize == CS6)
		ctrl |= data_6bit;
	else if (chsize == CS5)
		ctrl |= data_5bit;

/* If stopb is true we set STB which means 2 stop-bits */
/* otherwise we only get 1 stop-bit.                   */

	ctrl |= (stopb ? STB : 0);
	ctrl |= ((parity & PARENB) ? ((parity & PARODD) ? (PEN) : (PEN |
							  EPS)) : 0x00 );

	uart.LCR = (ctrl | DLAB);

		/* Store high byte of divisor */

	uart.DLM = ((div >> 8) & 0xff);

		/* Store low byte of divisor */

	uart.DLL = (div & 0xff);
	uart.LCR = ctrl;
}

/***** end of ser_change_speed() *****/


/***** start of ser_throttle() *****/

static void ser_throttle(struct async_struct *info, int status)
{
	if (status)	ser_RTSoff;
	else		ser_RTSon;
}

/***** end of ser_throttle() *****/


/***** start of ser_set_break() *****/

static void ser_set_break(struct async_struct *info, int break_flag)
{
	if (break_flag)		uart.LCR |= SET_BREAK;
	else			uart.LCR &= ~SET_BREAK;
}

/***** end of ser_set_break() *****/


/***** start of ser_get_serial_info() *****/

static void ser_get_serial_info(struct async_struct *info,
				struct serial_struct *retinfo)
{
	retinfo->baud_base = WHIPPET_BAUD_BASE;
	retinfo->xmit_fifo_size = FIFO_SIZE;	/* This field is currently ignored, */
						/* by the upper layers of the       */
						/* serial-driver.                   */
	retinfo->custom_divisor = info->custom_divisor;
}

/***** end of ser_get_serial_info() *****/


/***** start of ser_get_modem() *****/

static unsigned int ser_get_modem_info(struct async_struct *info)
{
u_char msr, mcr;

	msr = uart.MSR;
	mcr = uart.MCR;		/* The DTR and RTS are located in the */
				/* ModemControlRegister ...           */

	return(
		((mcr & DTR) ? TIOCM_DTR : 0) |
		((mcr & RTS) ? TIOCM_RTS : 0) |

		((msr & DCD) ? 0 : TIOCM_CAR) | /* DCD is active low */
		((msr & CTS) ? TIOCM_CTS : 0) |
		((msr & DSR) ? TIOCM_DSR : 0) |
		((msr & RING_I) ? TIOCM_RNG : 0)
	);
}

/***** end of ser_get_modem() *****/


/***** start of ser_set_modem() *****/

static int ser_set_modem_info(struct async_struct *info, int new_dtr,
			      int new_rts)
{
	if (new_dtr == 0)		ser_DTRoff;
	else if (new_dtr == 1)		ser_DTRon;

	if (new_rts == 0)		ser_RTSoff;
	else	if (new_rts == 1)	ser_RTSon;

	return 0;
};

/***** end of ser_set_modem() *****/


/***** start of ser_stop_receive() *****/

static void ser_stop_receive (struct async_struct *info)
{
	/* Disable uart receive and status interrupts */
	uart.IER &= ~(ERDAI | ELSI | EMSI);
}

/***** end of ser_stop_receive() *****/


/***** start of ser_trans_empty() *****/

static int ser_trans_empty (struct async_struct *info)
{
	return (uart.LSR & THRE);
}

/***** end of ser_trans_empty() *****/


/***** start of whippet_init() *****/

/*
 * Detect and initialize any Whippet found in the system.
 */

int whippet_init(void)
{
struct serial_struct req;
struct async_struct *info;
unsigned char ch;
unsigned long flags;

	if (!((MACH_IS_AMIGA) && (boot_info.bi_amiga.model == AMI_1200)))
		return -ENODEV;

	save_flags(flags);
	cli();

/* Acknowledge any possible PCMCIA interrupts */

	ch=inb(GAYLE_IRQ_STATUS);
	outb(0xdc | (ch & 0x03),GAYLE_IRQ_STATUS);

/* Check for 16c550 by testing the scratch register and other stuff */
/* If there is nothing in the PCMCIA port, then any reads should(!) */
/* return 0xff.							    */

#ifdef WHIPPET_DEBUG
	printk("Probing for Whippet Serial...\n");
	printk("Looking at address 0x%08x\n",(int)&uart);
#endif
	uart.IER=0x00;
	uart.MCR=0x00;
	uart.LCR=0x00;
	uart.FCR=0x00;
	if ((uart.IER) != 0x00)	return -ENODEV;

	(void)uart.RBR;
	(void)uart.LSR;
	(void)uart.IIR;
	(void)uart.MSR;
	uart.FCR=0x00;
	uart.LCR=0x00;
	uart.MCR=0x00;

	uart.SCR=0xA5;
	uart.IER=0x00;
	if ((uart.SCR) != 0xA5)	return -ENODEV;

	uart.SCR=0x4D;
	uart.IER=0x00;
	if ((uart.SCR) != 0x4D)	return -ENODEV;

	restore_flags(flags);

/*
 * Set the necessary tty-stuff.
 */
	req.line = -1;			/* first free ttyS? device */
	req.type = SER_WHIPPET;
	req.port = (int) &uart.RBR;

	if ((line = register_serial( &req )) < 0)
	{
		printk( "Cannot register Whippet serial port: no free device\n" );
		return -EBUSY;
	}

	info = &rs_table[line];		/* set info == struct *async_struct */

	info->nr_uarts = 1;			/* one UART (necessary?) */
	info->sw = &whippet_ser_switch;		/* switch functions      */

	/* Install ISR - level 2 - data is struct *async_struct */

	request_irq(IRQ_AMIGA_PORTS, ser_interrupt, 0, "whippet serial", info);


/* Add {} in here so that without debugging we still get the
 * desired effect (murray)
 */

/* Wait for the uarts to get empty */

	while(!((uart.LSR) & TEMT))
	{
#if WHIPPET_DEBUG
	printk("Waiting for transmitter to finish\n");
#endif
	}

/*
 * Set the uarts to a default setting of 8N1 - 9600
 */

	uart.LCR = (data_8bit | DLAB);
	uart.DLM = 0;
	uart.DLL = 48;
	uart.LCR = (data_8bit);

/*
 * Enable + reset both the tx and rx FIFO's.
 * Set the rx FIFO-trigger count.
 */

	uart.FCR = (FIFO_ENA | RCVR_FIFO_RES | XMIT_FIFO_RES | FIFO_TRIGGER_LEVEL );

/*
 * Disable all uart interrupts (they will be re-enabled in ser_init when
 *  they are needed).
 */
	uart.IER = 0x00;


/* Print confirmation of whippet detection */

	printk("Detected Whippet Serial Port at 0x%08x (ttyS%i)\n",(int)&uart,line);
	return(0);
}

/***** end of whippet_init() *****/
