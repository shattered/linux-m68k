

/*
 * drivers/char/atari_MFPser.c: Atari MFP serial ports implementation
 *
 * Copyright 1994 Roman Hodek <Roman.Hodek@informatik.uni-erlangen.de>
 * Partially based on PC-Linux serial.c by Linus Torvalds and Theodore Ts'o
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file COPYING in the main directory of this archive
 * for more details.
 *
 */


/* This file implements the MFP serial ports. These come in two
 * flavors: with or without control lines (RTS, CTS, DTR, ...). They
 * are distinguished by having two different types, MFP_CTRL and
 * MFP_BARE, resp. Most of the low-level functions are the same for
 * both, but some differ.
 *
 * Note that some assumptions are made about where to access the
 * control lines. If the port type is MFP_CTRL, the input lines (CTS
 * and DCD) are assumed to be in the MFP GPIP register, bits 1 and 2.
 * The output lines (DTR and RTS) have to be in the Soundchip Port A,
 * bits 3 and 4. This is the standard ST/TT assigment. If Atari will
 * build a serial port in future, that uses other registers, you have
 * to rewrite this code. But if the port type is MFP_BARE, no such
 * assumptions are necessary. All registers needed are fixed by the
 * MFP hardware. The only parameter is the MFP base address. This is
 * used to implement Serial1 for the TT and the (not connected) MFP
 * port of the Falcon.
 *
 */

#include <linux/module.h>

#include <linux/types.h>
#include <linux/sched.h>
#include <linux/interrupt.h>
#include <linux/errno.h>
#include <linux/tty.h>
#include <linux/termios.h>
#include <linux/serial.h>

#include <asm/setup.h>
#include <asm/atarihw.h>
#include <asm/atariints.h>
#include <asm/irq.h>

#include "atari_MFPser.h"



/***************************** Prototypes *****************************/

static void MFPser_init_port( struct async_struct *info, int type,
			     int tt_flag );
#ifdef MODULE
static void MFPser_deinit_port( struct async_struct *info, int tt_flag );
#endif
static void MFPser_rx_int (int irq, void *data, struct pt_regs *fp);
static void MFPser_rxerr_int (int irq, void *data, struct pt_regs *fp);
static void MFPser_tx_int (int irq, void *data, struct pt_regs *fp);
static void MFPctrl_dcd_int (int irq, void *data, struct pt_regs *fp);
static void MFPctrl_cts_int (int irq, void *data, struct pt_regs *fp);
static void MFPctrl_ri_int (int irq, void *data, struct pt_regs *fp);
static void MFPser_init( struct async_struct *info );
static void MFPser_deinit( struct async_struct *info, int leave_dtr );
static void MFPser_enab_tx_int( struct async_struct *info, int enab_flag );
static int MFPser_check_custom_divisor (struct async_struct *info,
					int baud_base, int divisor);
static void MFPser_change_speed( struct async_struct *info );
static void MFPctrl_throttle( struct async_struct *info, int status );
static void MFPbare_throttle( struct async_struct *info, int status );
static void MFPser_set_break( struct async_struct *info, int break_flag );
static void MFPser_get_serial_info( struct async_struct *info, struct
                                    serial_struct *retinfo );
static unsigned int MFPctrl_get_modem_info( struct async_struct *info );
static unsigned int MFPbare_get_modem_info( struct async_struct *info );
static int MFPctrl_set_modem_info( struct async_struct *info, int new_dtr,
                                   int new_rts );
static int MFPbare_set_modem_info( struct async_struct *info, int new_dtr,
                                   int new_rts );
static void MFPser_stop_receive (struct async_struct *info);
static int MFPser_trans_empty (struct async_struct *info);

/************************* End of Prototypes **************************/



/* SERIALSWITCH structures for MFP ports
 * Most functions are common to MFP ports with or without control lines
 */

static SERIALSWITCH MFPctrl_switch = {
	MFPser_init, MFPser_deinit, MFPser_enab_tx_int,
	MFPser_check_custom_divisor, MFPser_change_speed,
	MFPctrl_throttle, MFPser_set_break,
	MFPser_get_serial_info, MFPctrl_get_modem_info,
	MFPctrl_set_modem_info, NULL, MFPser_stop_receive, MFPser_trans_empty,
	NULL
};


static SERIALSWITCH MFPbare_switch = {
	MFPser_init, MFPser_deinit, MFPser_enab_tx_int,
	MFPser_check_custom_divisor, MFPser_change_speed,
	MFPbare_throttle, MFPser_set_break,
	MFPser_get_serial_info, MFPbare_get_modem_info,
	MFPbare_set_modem_info, NULL, MFPser_stop_receive, MFPser_trans_empty,
	NULL
};

/* MFP Timer Modes divided by 2 (this already done in the BAUD_BASE */
int MFP_timer_modes[] = { 2, 5, 8, 25, 32, 50, 100 };

/* Divisors for standard speeds */
int MFP_baud_table[15] = {
	/* B0     */ 0,
	/* B50    */ 768,
	/* B75    */ 512,
	/* B110   */ 350, /* really 109.71 bps */
	/* B134	  */ 286, /* really 134.27 bps */
	/* B150	  */ 256,
	/* B200	  */ 192,
	/* B300	  */ 128,
	/* B600	  */ 64,
	/* B1200  */ 32,
	/* B1800  */ 21,
	/* B2400  */ 16,
	/* B4800  */ 8,
	/* B9600  */ 4,
	/* B19200 */ 2
};


#define DEFAULT_STMFP_LINE	0	/* ttyS0 */
#define DEFAULT_TTMFP_LINE	2	/* ttyS2 */

static int stmfp_line = -1, ttmfp_line = -1;


int atari_MFPser_init( void )

{	struct serial_struct req;
	int nr = 0;
	extern char m68k_debug_device[];

	if (!MACH_IS_ATARI)
		return( -ENODEV );

	if (ATARIHW_PRESENT(ST_MFP)) {
		if (!strcmp( m68k_debug_device, "ser1" ))
			printk(KERN_NOTICE "ST-MFP serial port used as debug device\n" );
		else {
			req.line = DEFAULT_STMFP_LINE;
			req.type = SER_MFP_CTRL;
			req.port = (int)&mfp;
			if ((stmfp_line = register_serial( &req )) >= 0) {
				MFPser_init_port( &rs_table[stmfp_line], req.type, 0 );
				++nr;
			}
			else
				printk(KERN_WARNING "Cannot allocate ttyS%d for ST-MFP\n", req.line );
		}
	}

	if (ATARIHW_PRESENT(TT_MFP)) {
		req.line = DEFAULT_TTMFP_LINE;
		req.type = SER_MFP_BARE;
		req.port = (int)&tt_mfp;
		if ((ttmfp_line = register_serial( &req )) >= 0) {
			MFPser_init_port( &rs_table[ttmfp_line], req.type, 1 );
			++nr;
		}
		else
			printk(KERN_WARNING "Cannot allocate ttyS%d for TT-MFP\n", req.line );
	}

	return( nr > 0 ? 0 : -ENODEV );
}


static void MFPser_init_port( struct async_struct *info, int type, int tt_flag)
{
	/* set ISRs, but don't enable interrupts yet (done in init());
	 * all ints are choosen of type FAST, and they're really quite fast.
	 * Furthermore, we have to account for the fact that these are three ints,
	 * and one can interrupt another. So better protect them against one
	 * another...
	 */
	request_irq(tt_flag ? IRQ_TT_MFP_SEREMPT : IRQ_MFP_SEREMPT,
	            MFPser_tx_int, IRQ_TYPE_FAST,
	            tt_flag ? "TT-MFP TX" : "ST-MFP TX", info);
	request_irq(tt_flag ? IRQ_TT_MFP_RECFULL : IRQ_MFP_RECFULL,
	            MFPser_rx_int, IRQ_TYPE_FAST,
	            tt_flag ? "TT-MFP RX" : "ST-MFP RX", info);
	request_irq(tt_flag ? IRQ_TT_MFP_RECERR : IRQ_MFP_RECERR,
	            MFPser_rxerr_int, IRQ_TYPE_FAST,
	            tt_flag ? "TT-MFP RX error" : "ST-MFP RX error", info);
	/* Tx_err interrupt unused (it signals only that the Tx shift reg
	 * is empty)
	 */

	if (type == SER_MFP_CTRL && !tt_flag) {
		/* The DCD, CTS and RI ints are slow ints, because I
		   see no races with the other ints */
		request_irq(IRQ_MFP_DCD, MFPctrl_dcd_int, IRQ_TYPE_SLOW,
		            "ST-MFP DCD", info);
		request_irq(IRQ_MFP_CTS, MFPctrl_cts_int, IRQ_TYPE_SLOW,
		            "ST-MFP CTS", info);
		request_irq(IRQ_MFP_RI, MFPctrl_ri_int, IRQ_TYPE_SLOW,
		            "ST-MFP RI", info);
		/* clear RTS and DTR */
		GIACCESS( GI_RTS | GI_DTR );
	}

	info->sw = (type == SER_MFP_CTRL ? &MFPctrl_switch : &MFPbare_switch);

	currMFP(info)->rcv_stat  = 0;	/* disable Rx */
	currMFP(info)->trn_stat  = 0;	/* disable Tx */
}


#ifdef MODULE
static void MFPser_deinit_port( struct async_struct *info, int tt_flag )

{
	free_irq(tt_flag ? IRQ_TT_MFP_SEREMPT : IRQ_MFP_SEREMPT, info);
	free_irq(tt_flag ? IRQ_TT_MFP_RECFULL : IRQ_MFP_RECFULL, info);
	free_irq(tt_flag ? IRQ_TT_MFP_RECERR : IRQ_MFP_RECERR, info);
	if (info->type == SER_MFP_CTRL && !tt_flag) {
		free_irq(IRQ_MFP_DCD, info );
		free_irq(IRQ_MFP_CTS, info );
		free_irq(IRQ_MFP_RI, info);
	}
}
#endif


static void MFPser_rx_int( int irq, void *data, struct pt_regs *fp)
{
	struct async_struct *info = data;
	int		ch, stat, err;
/*	unsigned long flags; */

	stat = currMFP(info)->rcv_stat;
	ch   = currMFP(info)->usart_dta;
	/* Frame Errors don't cause a RxErr IRQ! */
	err  = (stat & RSR_FRAME_ERR) ? TTY_FRAME : 0;

/*	save_flags(flags);
	cli(); */
	rs_receive_char (info, ch, err);
/*	restore_flags(flags); */
}


static void MFPser_rxerr_int( int irq, void *data, struct pt_regs *fp)
{
	struct async_struct *info = data;
	int		ch, stat, err;
/*	unsigned long flags; */

	stat = currMFP(info)->rcv_stat;
	ch   = currMFP(info)->usart_dta; /* most probably junk data */

	if (stat & RSR_PARITY_ERR)
		err = TTY_PARITY;
	else if (stat & RSR_OVERRUN_ERR)
		err = TTY_OVERRUN;
	else if (stat & RSR_BREAK_DETECT)
		err = TTY_BREAK;
	else if (stat & RSR_FRAME_ERR)	/* should not be needed */
		err = TTY_FRAME;
	else
		err = 0;

/*	save_flags(flags);
	cli(); */
	rs_receive_char (info, ch, err);
/*	restore_flags(flags); */
}


static void MFPser_tx_int( int irq, void *data, struct pt_regs *fp)
{
	struct async_struct *info = data;
	int ch;

	if (currMFP(info)->trn_stat & TSR_BUF_EMPTY) {

		if ((ch = rs_get_tx_char( info )) >= 0)
			currMFP(info)->usart_dta = ch;
		if (ch == -1 || rs_no_more_tx( info ))
			/* disable tx interrupts */
			currMFP(info)->int_en_a &= ~0x04;
	}
}


static void MFPctrl_dcd_int( int irq, void *data, struct pt_regs *fp)
{
	struct async_struct *info = data;

	/* Toggle active edge to get next change of DCD! */
	currMFP(info)->active_edge ^= GPIP_DCD;

	rs_dcd_changed( info, !(currMFP(info)->par_dt_reg & GPIP_DCD) );
}


static void MFPctrl_cts_int( int irq, void *data, struct pt_regs *fp)
{
	struct async_struct *info = data;

	/* Toggle active edge to get next change of CTS! */
	currMFP(info)->active_edge ^= GPIP_CTS;

	rs_check_cts( info, !(currMFP(info)->par_dt_reg & GPIP_CTS) );
}


static void MFPctrl_ri_int(int irq, void *data, struct pt_regs *fp)
{
	struct async_struct *info = data;
	/* update input line counter */
	info->icount.rng++;
	wake_up_interruptible(&info->delta_msr_wait);
}


static void MFPser_init( struct async_struct *info )
{
	/* base value for UCR */
	currMFP(info)->usart_ctr = (UCR_PARITY_OFF | UCR_ASYNC_1 |
				    UCR_CHSIZE_8 | UCR_PREDIV);

	/* enable Rx and clear any error conditions */
	currMFP(info)->rcv_stat = RSR_RX_ENAB;

	/* enable Tx */
	currMFP(info)->trn_stat = TSR_TX_ENAB;

	/* enable Rx, RxErr and Tx interrupts */
	currMFP(info)->int_en_a |= 0x1c;
	currMFP(info)->int_mk_a |= 0x1c;

	if (info->type == SER_MFP_CTRL) {

		int status;

		/* set RTS and DTR (low-active!) */
		GIACCESS( ~(GI_RTS | GI_DTR) );

		/* Set active edge of CTS and DCD signals depending on their
		 * current state.
		 * If the line status changes between reading the status and
		 * enabling the interrupt, this won't work :-( How could it be
		 * done better??
		 * ++andreas: do it better by looping until stable
		 */
		do {
		    status = currMFP(info)->par_dt_reg & GPIP_CTS;
		    if (status)
			currMFP(info)->active_edge &= ~GPIP_CTS;
		    else
			currMFP(info)->active_edge |= GPIP_CTS;
		} while ((currMFP(info)->par_dt_reg & GPIP_CTS) != status);

		do {
		    status = currMFP(info)->par_dt_reg & GPIP_DCD;
		    if (status)
			currMFP(info)->active_edge &= ~GPIP_DCD;
		    else
			currMFP(info)->active_edge |= GPIP_DCD;
		} while ((currMFP(info)->par_dt_reg & GPIP_DCD) != status);

		/* enable CTS and DCD interrupts */
		currMFP(info)->int_en_b |= 0x06;
		currMFP(info)->int_mk_b |= 0x06;
	}
	MOD_INC_USE_COUNT;
}


static void MFPser_deinit( struct async_struct *info, int leave_dtr )
{
	/* disable Rx, RxErr and Tx interrupts */
	currMFP(info)->int_en_a &= ~0x1c;

	if (info->type == SER_MFP_CTRL) {
		/* disable CTS and DCD interrupts */
		currMFP(info)->int_en_b &= ~0x06;
	}

	/* disable Rx and Tx */
	currMFP(info)->rcv_stat = 0;
	currMFP(info)->trn_stat = 0;

	/* wait for last byte to be completely shifted out */
	while( !(currMFP(info)->trn_stat & TSR_LAST_BYTE_SENT) )
		;

	if (info->type == SER_MFP_CTRL) {
		/* drop RTS and DTR if required */
		MFPser_RTSoff();
		if (!leave_dtr)
			MFPser_DTRoff();
	}

	/* read Rx status and data to clean up */
	(void)currMFP(info)->rcv_stat;
	(void)currMFP(info)->usart_dta;
	MOD_DEC_USE_COUNT;
}


static void MFPser_enab_tx_int( struct async_struct *info, int enab_flag )
{
	if (enab_flag) {
		unsigned long flags;
		currMFP(info)->int_en_a |= 0x04;
		save_flags(flags);
		cli();
		MFPser_tx_int (0, info, 0);
		restore_flags(flags);
	}
	else
		currMFP(info)->int_en_a &= ~0x04;
}


static int MFPser_check_custom_divisor (struct async_struct *info,
					int baud_base, int divisor)
{
	int		i;

	if (baud_base != MFP_BAUD_BASE)
		return -1;

	/* divisor must be a multiple of 2 or 5 (because of timer modes) */
	if (divisor == 0 || ((divisor & 1) && (divisor % 5) != 0)) return( -1 );

	/* Determine what timer mode would be selected and look if the
	 * timer value would be greater than 256
	 */
	for( i = sizeof(MFP_timer_modes)/sizeof(*MFP_timer_modes)-1; i >= 0; --i )
		if (divisor % MFP_timer_modes[i] == 0) break;
	if (i < 0) return( -1 ); /* no suitable timer mode found */

	return (divisor / MFP_timer_modes[i] > 256);
}


static void MFPser_change_speed( struct async_struct *info )
{
	unsigned	cflag, baud, chsize, stopb, parity, aflags;
	unsigned	div = 0, timer_val;
	int			timer_mode;
	unsigned long ipl;

	if (!info->tty || !info->tty->termios) return;

	cflag  = info->tty->termios->c_cflag;
	baud   = cflag & CBAUD;
	chsize = cflag & CSIZE;
	stopb  = cflag & CSTOPB;
	parity = cflag & (PARENB | PARODD);
	aflags = info->flags & ASYNC_SPD_MASK;

	if (cflag & CRTSCTS)
		info->flags |= ASYNC_CTS_FLOW;
	else
		info->flags &= ~ASYNC_CTS_FLOW;
	if (cflag & CLOCAL)
		info->flags &= ~ASYNC_CHECK_CD;
	else
		info->flags |= ASYNC_CHECK_CD;

	if (baud & CBAUDEX) {
		baud &= ~CBAUDEX;
		if (baud < 1 || baud > 2)
			info->tty->termios->c_cflag &= ~CBAUDEX;
		else
			baud += 15;
	}
	if (baud == 15) {
		if (aflags == ASYNC_SPD_HI)
			baud += 1;
		if (aflags == ASYNC_SPD_VHI)
			baud += 2;
		if (aflags == ASYNC_SPD_CUST)
			div = info->custom_divisor;
	}
	if (!div) {
		/* Maximum MFP speed is 19200 :-( */
		if (baud > 14) baud = 14;
		div = MFP_baud_table[baud];
	}

	if (div) {
		/* turn on DTR */
		MFPser_DTRon ();
	} else {
		/* speed == 0 -> drop DTR */
		MFPser_DTRoff();
		return;
	}

	/* compute timer value and timer mode (garuateed to succeed, because
	 * the divisor was checked before by check_custom_divisor(), if it
	 * is used-supplied)
	 */
	for( timer_mode = sizeof(MFP_timer_modes)/sizeof(*MFP_timer_modes)-1;
		 timer_mode >= 0; --timer_mode )
		if (div % MFP_timer_modes[timer_mode] == 0) break;
	timer_val = div / MFP_timer_modes[timer_mode];

	save_flags (ipl);
	cli();
	/* disable Rx and Tx while changing parameters */
	currMFP(info)->rcv_stat = 0;
	currMFP(info)->trn_stat = 0;

	/* stop timer D to set new timer value immediatly after re-enabling */
	currMFP(info)->tim_ct_cd &= ~0x07;
	currMFP(info)->tim_dt_d = timer_val;
	currMFP(info)->tim_ct_cd |= (timer_mode+1);

	currMFP(info)->usart_ctr =
		( (parity & PARENB) ?
		      ((parity & PARODD) ? UCR_PARITY_ODD : UCR_PARITY_EVEN) :
		      UCR_PARITY_OFF ) |
		( chsize == CS5 ? UCR_CHSIZE_5 :
		  chsize == CS6 ? UCR_CHSIZE_6 :
		  chsize == CS7 ? UCR_CHSIZE_7 :
						  UCR_CHSIZE_8 ) |
		( stopb ? UCR_ASYNC_2 : UCR_ASYNC_1 ) |
		UCR_PREDIV;

	/* re-enable Rx and Tx */
	currMFP(info)->rcv_stat = RSR_RX_ENAB;
	currMFP(info)->trn_stat = TSR_TX_ENAB;
	restore_flags (ipl);
}

static void MFPctrl_throttle( struct async_struct *info, int status )
{
	if (status)
		MFPser_RTSoff();
	else
		MFPser_RTSon();
}


static void MFPbare_throttle( struct async_struct *info, int status )
{
	/* no-op */
}


static void MFPser_set_break( struct async_struct *info, int break_flag )
{
	if (break_flag)
		currMFP(info)->trn_stat |= TSR_SEND_BREAK;
	else
		currMFP(info)->trn_stat &= ~TSR_SEND_BREAK;
}


static void MFPser_get_serial_info( struct async_struct *info,
				   struct serial_struct *retinfo )
{
	retinfo->baud_base = MFP_BAUD_BASE;
	retinfo->custom_divisor = info->custom_divisor;
}


static unsigned int MFPctrl_get_modem_info( struct async_struct *info )
{
	unsigned	gpip, gi;
	unsigned int ri;
	unsigned long ipl;

	save_flags (ipl);
	cli();
	gpip = currMFP(info)->par_dt_reg;
	gi   = GIACCESS( 0 );
	restore_flags (ipl);

	/* DSR is not connected on the Atari, assume it to be set;
	 * RI is tested by the RI bitpos field of info, because the RI is
	 * signalled at different ports on TT and Falcon
	 * ++andreas: the signals are inverted!
	 */
	/* If there is a SCC but no TT_MFP then RI on the ST_MFP is
	   used for SCC channel b */
	if (ATARIHW_PRESENT (SCC) && !ATARIHW_PRESENT (TT_MFP))
		ri = 0;
	else if (currMFP(info) == &mfp)
		ri = gpip & GPIP_RI ? 0 : TIOCM_RNG;
	else
		ri = 0;
	return (((gi   & GI_RTS  ) ? 0 : TIOCM_RTS) |
		((gi   & GI_DTR  ) ? 0 : TIOCM_DTR) |
		((gpip & GPIP_DCD) ? 0 : TIOCM_CAR) |
		((gpip & GPIP_CTS) ? 0 : TIOCM_CTS) |
		TIOCM_DSR | ri);
}


static unsigned int MFPbare_get_modem_info( struct async_struct *info )
{
	return( TIOCM_RTS | TIOCM_DTR | TIOCM_CAR | TIOCM_CTS | TIOCM_DSR );
}


static int MFPctrl_set_modem_info( struct async_struct *info,
				  int new_dtr, int new_rts )
{
	if (new_dtr == 0)
		MFPser_DTRoff();
	else if (new_dtr == 1)
		MFPser_DTRon();

	if (new_rts == 0)
		MFPser_RTSoff();
	else if (new_rts == 1)
		MFPser_RTSon();

	return( 0 );
}


static int MFPbare_set_modem_info( struct async_struct *info,
				  int new_dtr, int new_rts )
{
	/* no-op */

	/* Is it right to return an error or should the attempt to change
	 * DTR or RTS be silently ignored?
	 */
	return( -EINVAL );
}

static void MFPser_stop_receive (struct async_struct *info)
{
	/* disable rx and rxerr interrupt */
	currMFP (info)->int_en_a &= ~0x18;

	/* disable receiver */
	currMFP (info)->rcv_stat = 0;
	/* disable transmitter */
	currMFP (info)->trn_stat = 0;
}

static int MFPser_trans_empty (struct async_struct *info)
{
	return (currMFP (info)->trn_stat & TSR_LAST_BYTE_SENT) != 0;
}

#ifdef MODULE
int init_module(void)
{
	return( atari_MFPser_init() );
}

void cleanup_module(void)
{
	if (stmfp_line >= 0) {
		MFPser_deinit_port( &rs_table[stmfp_line], 0 );
		unregister_serial( stmfp_line );
	}
	if (ttmfp_line >= 0) {
		MFPser_deinit_port( &rs_table[ttmfp_line], 1 );
		unregister_serial( ttmfp_line );
	}
}
#endif
