

/*
 * drivers/char/atari_MIDI.c: Atari MIDI driver as serial port
 *
 * Copyright 1994 Roman Hodek <Roman.Hodek@informatik.uni-erlangen.de>
 * Partially based on PC-Linux serial.c by Linus Torvalds and Theodore Ts'o
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file COPYING in the main directory of this archive
 * for more details.
 *
 * Modified for midi by Martin Schaller
 */


#include <linux/module.h>

#include <linux/types.h>
#include <linux/sched.h>
#include <linux/interrupt.h>
#include <linux/errno.h>
#include <linux/tty.h>
#include <linux/termios.h>
#include <linux/serial.h>

#include <asm/traps.h>
#include <asm/atarihw.h>
#include <asm/atariints.h>
#include <asm/atarikb.h>

#include "atari_MFPser.h"

struct async_struct *midi_info;

#define _BHA_MIDI_SHADOW

#define DEFAULT_MIDI_LINE	5	/* ttyS5 */

static unsigned char mid_ctrl_shadow;	/* Bernd Harries, 960525 */

static void MIDI_int(void)	/* called from keyboard int handler */

{	int		ch, stat, err;

	stat = acia.mid_ctrl;

	if (stat & (ACIA_RDRF | ACIA_FE | ACIA_OVRN)) {
		ch = acia.mid_data;
		err=0;
		if (stat & ACIA_FE)
			err = TTY_FRAME;
		if (stat & ACIA_OVRN)
			err = TTY_OVERRUN;
		rs_receive_char (midi_info, ch, err);
	}
	if (acia.mid_ctrl & ACIA_TDRE) {

		if ((ch = rs_get_tx_char( midi_info )) >= 0)
			acia.mid_data = ch;
		if (ch == -1 || rs_no_more_tx( midi_info ))
			/* disable tx interrupts */
			mid_ctrl_shadow &= ~ACIA_RLTIE;		/* RTS Low, Tx IRQ Disabled */

#ifdef _BHA_MIDI_SHADOW
			acia.mid_ctrl = mid_ctrl_shadow;
#else
			acia.mid_ctrl = ACIA_DIV16 | ACIA_D8N1S | ACIA_RIE;
#endif
	}
}

static void MIDI_init( struct async_struct *info )

{
	/* Baud = DIV16, 8N1, denable rx interrupts */
	mid_ctrl_shadow = ACIA_DIV16 | ACIA_D8N1S | ACIA_RIE;
	acia.mid_ctrl = mid_ctrl_shadow;

	MOD_INC_USE_COUNT;
}


static void MIDI_deinit( struct async_struct *info, int leave_dtr )

{
	/* Baud = DIV16, 8N1, disable Rx and Tx interrupts */
	mid_ctrl_shadow = ACIA_DIV16 | ACIA_D8N1S;
	acia.mid_ctrl = mid_ctrl_shadow;

	/* read Rx status and data to clean up */

	(void)acia.mid_ctrl;
	(void)acia.mid_data;
	MOD_DEC_USE_COUNT;
}


static void MIDI_enab_tx_int( struct async_struct *info, int enab_flag )
{
/*
   ACIA Control Register can only be written! Read accesses Status Register!
   Shadowing may be nescessary here like for SCC

   Bernd Harries, 960525  Tel.: +49-421-804309
   harries@asrv01.atlas.de
   Bernd_Harries@hb2.maus.de
 */

	if (enab_flag) {
		unsigned long flags;
		int ch;

		save_flags(flags);
		cli();

		mid_ctrl_shadow |= ACIA_RLTIE;	/* RTS Low, Tx IRQ Enabled */

#ifdef _BHA_MIDI_SHADOW
		acia.mid_ctrl = mid_ctrl_shadow;
#else
		acia.mid_ctrl = ACIA_DIV16 | ACIA_D8N1S | ACIA_RIE | ACIA_RLTIE;
#endif
		/* restart the transmitter */

		if (acia.mid_ctrl & ACIA_TDRE) {	/* If last Char since disabling is gone */
			if ((ch = rs_get_tx_char( midi_info )) >= 0)
				acia.mid_data = ch;
			if (ch == -1 || rs_no_more_tx( midi_info )){
				/* disable tx interrupts */
				mid_ctrl_shadow &= ~ACIA_RLTIE;		/* RTS Low, Tx IRQ Disabled */

#ifdef _BHA_MIDI_SHADOW
				acia.mid_ctrl = mid_ctrl_shadow;
#else
				acia.mid_ctrl = ACIA_DIV16 | ACIA_D8N1S | ACIA_RIE;
#endif
			}
		}
		restore_flags(flags);
	} else {
		mid_ctrl_shadow &= ~ACIA_RLTIE;		/* RTS Low, Tx IRQ Disabled */

#ifdef _BHA_MIDI_SHADOW
		acia.mid_ctrl = mid_ctrl_shadow;
#else
		acia.mid_ctrl = ACIA_DIV16 | ACIA_D8N1S | ACIA_RIE;
#endif

	}
}


static int MIDI_check_custom_divisor( struct async_struct *info,
				     int baud_base, int divisor )

{
	return (-1);
}

static void MIDI_change_speed( struct async_struct *info )

{
	/* no-op */
}


static void MIDI_throttle( struct async_struct *info, int status )

{
	/* no-op */
}


static void MIDI_set_break( struct async_struct *info, int break_flag )

{
	/* no-op */
}

static unsigned int MIDI_get_modem_info( struct async_struct *info )

{
	return( TIOCM_RTS | TIOCM_DTR | TIOCM_CAR | TIOCM_CTS | TIOCM_DSR );
}


static void MIDI_get_serial_info( struct async_struct *info,
								   struct serial_struct *retinfo )

{
	retinfo->baud_base = 31250;
	retinfo->custom_divisor = 1;
}

static int MIDI_set_modem_info( struct async_struct *info,
				int new_dtr, int new_rts )

{
	/* no-op */

	/* Is it right to return an error or should the attempt to change
	 * DTR or RTS be silently ignored?
	 */
	return( -EINVAL );
}

static void MIDI_stop_receive (struct async_struct *info)
{
	/* disable receive interrupts */
	mid_ctrl_shadow &= ACIA_RIE;

#ifdef _BHA_MIDI_SHADOW
	acia.mid_ctrl = mid_ctrl_shadow;
#else
	acia.mid_ctrl = ACIA_DIV16 | ACIA_D8N1S;
#endif
}

static int MIDI_trans_empty (struct async_struct *info)
{
	return (acia.mid_ctrl & ACIA_TDRE) != 0;
}

/* SERIALSWITCH structures for MIDI port
 */

static SERIALSWITCH MIDI_switch = {
	MIDI_init, MIDI_deinit, MIDI_enab_tx_int,
	MIDI_check_custom_divisor, MIDI_change_speed,
	MIDI_throttle, MIDI_set_break,
	MIDI_get_serial_info, MIDI_get_modem_info,
	MIDI_set_modem_info, NULL, MIDI_stop_receive, MIDI_trans_empty, NULL
};

int atari_MIDI_init( void )

{	struct serial_struct req;
	int line;

	req.line = DEFAULT_MIDI_LINE;
	req.type = SER_MIDI;
	req.port = (int) &acia.mid_ctrl;
	if ((line = register_serial( &req )) < 0) {
		printk(KERN_WARNING "Cannot allocate ttyS%d for MIDI\n", req.line );
		return line;
	}

	midi_info = &rs_table[line];
	midi_info->sw = &MIDI_switch;

	mid_ctrl_shadow = ACIA_DIV16 | ACIA_D8N1S;
	acia.mid_ctrl = mid_ctrl_shadow;

	atari_MIDI_interrupt_hook = MIDI_int;

	return( 0 );
}

#ifdef MODULE
int init_module(void)
{
	return( atari_MIDI_init() );
}

void cleanup_module(void)
{
	atari_MIDI_interrupt_hook = NULL;
	unregister_serial( midi_info->line );
}
#endif
