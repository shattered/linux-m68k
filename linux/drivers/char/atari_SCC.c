/*
 * drivers/char/atari_SCC.c: Atari SCC serial ports implementation
 *
 * Copyright 1994-95 Roman Hodek <Roman.Hodek@informatik.uni-erlangen.de>
 * Partially based on PC-Linux serial.c by Linus Torvalds and Theodore Ts'o
 *
 * Some parts were taken from the (incomplete) SCC driver by Lars Brinkhoff
 * <f93labr@dd.chalmers.se>
 *
 * Adapted to 1.2 by Andreas Schwab
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file COPYING in the main directory of this archive
 * for more details.
 *
 * Description/Notes:
 *
 *  - This driver currently handles the asynchronous modes of the SCC ports
 *    only. Synchronous operation or packet modes aren't implemented yet.
 *
 *  - Since there are many variations how the SCC can be integrated, the
 *    driver offers the possibility to provide the frequencies attached to the
 *    various clock inputs via an ioctl (along with an externally calculated
 *    baud table).
 *
 *  - I haven't spent much time for optimizations yet...
 *
 *  - Sorry, DMA for channel A in the TT isn't supported yet.
 *
 *  - Channel A is accessible via two different devices: ttyS3 and ttyS4. The
 *    former is the RS232 "Serial2" port, the latter the RS422 "LAN" port.
 *    Only one of these devices can be open at one time.
 *
 */

#include <linux/module.h>

#include <linux/types.h>
#include <linux/sched.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/tty.h>
#include <linux/termios.h>
#include <linux/serial.h>

#include <asm/setup.h>
#include <asm/atarihw.h>
#include <asm/atariints.h>
#include <asm/irq.h>
#include <asm/atari_SCCserial.h>

#include "atari_SCC.h"

#define	DEBUG_INT	0x01
#define	DEBUG_INIT	0x02
#define	DEBUG_THROTTLE	0x04
#define	DEBUG_INFO	0x08
#define	DEBUG_SPEED	0x10
#define	DEBUG_OPEN	0x20
#define	DEBUG_OVERRUNS	0x40

#define	DEBUG_ALL	0xffffffff
#define	DEBUG_NONE	0

#define	DEBUG	( DEBUG_NONE )

#define CHANNEL_A	0
#define CHANNEL_B	1


/* Shadows for all SCC write registers */
static unsigned char SCC_shadow[2][16];

/* To keep track of STATUS_REG state for detection of Ext/Status int source */
static unsigned char SCC_last_status_reg[2];

/* This array tells the clocks connected to RTxC or TRxC, resp., (2nd
 * index) for each channel (1st index).
 *
 * This table is initialzed for the TT. If we run on another machine,
 * the values are changed by the initialization function.
 */

static unsigned SCC_clocks[2][2] = {
	  /* RTxC */			/* TRxC */
	{ SCC_BAUD_BASE_PCLK4,	SCC_BAUD_BASE_NONE },	/* Channel A */
	{ SCC_BAUD_BASE_TIMC,	SCC_BAUD_BASE_BCLK }	/* Channel B */
};

/* The SCC's master clock (as variable, in case someone has unusual
 * hardware)
 */

static unsigned SCC_PCLK = SCC_BAUD_BASE_PCLK;


/* BRG values for the standard speeds and the various clock sources */

typedef struct {
	unsigned	clksrc;		/* clock source to use or -1 for not possible */
	unsigned	div;		/* divisor: 1, 2 and 4 correspond to
					 * direct 1:16, 1:32 and 1:64 modes,
					 * divisors >= 4 yield a BRG value of
					 * div/2-2 (in 1:16 mode)
					 */
} BAUD_ENTRY;

/* A pointer for each channel to the current baud table */
static BAUD_ENTRY *SCC_baud_table[2];

/* Baud table format:
 *
 * Each entry consists of the clock source (CLK_RTxC, CLK_TRxC or
 * CLK_PCLK) and a divisor. The following rules apply to the divisor:
 *
 *   - CLK_RTxC: 1 or even (1, 2 and 4 are the direct modes, > 4 use
 *               the BRG)
 *
 *   - CLK_TRxC: 1, 2 or 4 (no BRG, only direct modes possible)
 *
 *   - CLK_PCLK: >= 4 and even (no direct modes, only BRG)
 *
 */

/* This table is used if RTxC = 3.672 MHz. This is the case for TT's
 * channel A and for both channels on the Mega STE/Falcon. (TRxC is unused)
 */
static BAUD_ENTRY bdtab_norm[18] = {
	/* B0      */ { 0, 0 },
	/* B50     */ { CLK_RTxC, 4590 },
	/* B75     */ { CLK_RTxC, 3060 },
	/* B110    */ { CLK_PCLK, 4576 },
	/* B134    */ { CLK_PCLK, 3756 },
	/* B150    */ { CLK_RTxC, 1530 },
	/* B200    */ { CLK_PCLK, 2516 },
	/* B300    */ { CLK_PCLK, 1678 },
	/* B600    */ { CLK_PCLK, 838 },
	/* B1200   */ { CLK_PCLK, 420 },
	/* B1800   */ { CLK_PCLK, 280 },
	/* B2400   */ { CLK_PCLK, 210 },
	/* B4800   */ { CLK_RTxC, 48 },
	/* B9600   */ { CLK_RTxC, 24 },
	/* B19200  */ { CLK_RTxC, 12 },
	/* B38400  */ { CLK_RTxC, 6 },
	/* B57600  */ { CLK_RTxC, 4 },
	/* B115200 */ { CLK_RTxC, 2 }
};

/* This is a special table for the TT channel B with 307.2 kHz at RTxC
 * and 2.4576 MHz at TRxC
 */
static BAUD_ENTRY bdtab_TTChB[18] = {
	/* B0      */ { 0, 0 },
	/* B50     */ { CLK_RTxC, 384 },
	/* B75     */ { CLK_RTxC, 256 },
	/* B110    */ { CLK_PCLK, 4576 },
	/* B134    */ { CLK_PCLK, 3756 },
	/* B150    */ { CLK_RTxC, 128 },
	/* B200    */ { CLK_RTxC, 96 },
	/* B300    */ { CLK_RTxC, 64 },
	/* B600    */ { CLK_RTxC, 32 },
	/* B1200   */ { CLK_RTxC, 16 },
	/* B1800   */ { CLK_PCLK, 280 },
	/* B2400   */ { CLK_RTxC, 8 },
	/* B4800   */ { CLK_RTxC, 4 },
	/* B9600   */ { CLK_RTxC, 2 },
	/* B19200  */ { CLK_RTxC, 1 },
	/* B38400  */ { CLK_TRxC, 4 },
	/* B57600  */ { CLK_TRxC, 2 }, /* 57600 is not possible, use 76800 instead */
	/* B115200 */ { CLK_TRxC, 1 }  /* 115200 is not possible, use 153600 instead */
};

/* User settable tables */
static BAUD_ENTRY bdtab_usr[2][18];


/*
   Here are the values to compute the tables above. For each base
   clock, the BRG values for the common bps rates are listed. The
   divisor is (BRG+2)*2. For each clock, the 1:16 and 1:32 are also
   usable (and the BRG isn't used). 1:64 is the same as BRG with
   k==0. If more than clock source was possible for a bps rate I've
   choosen the one with the smallest error.

   For 307.2 kHz == base 19200:
     50    bps -> 190
     75    bps -> 126
     110   bps -> 85 (really 110.34 bps, error 0.31 %)
     134   bps -> 70 (really 133.33 bps, error 0.49 %)
     150   bps -> 62
     200   bps -> 46
     300   bps -> 30
     600   bps -> 14
     1200  bps -> 6
     1800  bps -> 3 (really 1920 bps, error 6.7 %)
     2400  bps -> 2
     4800  bps -> 0

   For 2.4576 MHz == base 153600:
     50    bps -> 1534
     75    bps -> 1022
     110   bps -> 696 (really 110.03 bps, error 0.027 %)
     134   bps -> 571 (really 134.03 bps, error 0.022 %)
     150   bps -> 510
     200   bps -> 382
     300   bps -> 254
     600   bps -> 126
     1200  bps -> 62
     1800  bps -> 41 (really 1786.1 bps, error 0.77 %)
     2400  bps -> 30
     4800  bps -> 14
     9600  bps -> 6
     19200 bps -> 2
     38400 bps -> 0

   For 3.672 MHz == base 229500:
     50    bps -> 2293
     75    bps -> 1528
     110   bps -> 1041
     134   bps -> 854
     150   bps -> 763
     200   bps -> 572
     300   bps -> 381
     600   bps -> 189
     1200  bps -> 94
     1800  bps -> 62
     2400  bps -> 46
     4800  bps -> 22
     9600  bps -> 10
     19200 bps -> 4
     38400 bps -> 1
     57600 bps -> 0

   For 8.053976 MHz == base 503374:
	  0    	 bps -> 0
	  50     bps -> 5032
	  75     bps -> 3354
	  110    bps -> 2286
	  134    bps -> 1876
	  150	 bps -> 1676
	  200	 bps -> 1256
	  300	 bps -> 837
	  600	 bps -> 417
	  1200   bps -> 208
	  1800   bps -> 138
	  2400   bps -> 103
	  4800   bps -> 50
	  9600   bps -> 24
	  19200  bps -> 11
	  31500  bps -> 6 (really 31461 bps)
	  50000  bps -> 3
	  125000 bps -> 0

*/


/* Is channel A switchable between two hardware ports? (Serial2/LAN) */

#define	SCCA_SWITCH_SERIAL2_ONLY	0	/* only connected to Serial2 */
#define	SCCA_SWITCH_LAN_ONLY		1	/* only connected to LAN */
#define	SCCA_SWITCH_BOTH		2	/* connected to both, switch by
						 * IO7 in the PSG */

static int SCC_chan_a_switchable;

/* Is channel A (two ports!) already open? */
static int SCC_chan_a_open;

/* For which line has channel A been opened? */
static int SCC_chan_a_line;

/* Are the register addresses for the channels reversed? (B before A). This is
 * the case for the ST_ESCC. */
static int ChannelsReversed;

/* This macro sets up the 'info' pointer for the interrupt functions of
 * channel A. It addresses the following problem: The isrs were registered
 * with callback_data == &rs_table[3] (= Serial2). But they can also be for
 * &rs_table[4] (LAN), if this port is the open one. SETUP_INFO() thus
 * advances the pointer if info->line == 3.
 */

#define DEFAULT_CHANNEL_B_LINE		1 /* ttyS1 */
#define DEFAULT_CHANNEL_A232_LINE	3 /* ttyS3 */
#define DEFAULT_CHANNEL_A422_LINE	4 /* ttyS4 */

static int chb_line = -1, cha232_line = -1, cha422_line = -1;


#define	SETUP_INFO(info)					\
	do {							\
		if (info->line == cha232_line &&		\
		    SCC_chan_a_line == cha422_line)		\
			info = &rs_table[cha422_line];		\
	} while(0)


/***************************** Prototypes *****************************/

static void SCC_init_port( struct async_struct *info, int type, int channel );
#ifdef MODULE
static void SCC_deinit_port( struct async_struct *info, int channel );
#endif
static void SCC_rx_int (int irq, void *data, struct pt_regs *fp);
static void SCC_spcond_int (int irq, void *data, struct pt_regs *fp);
static void SCC_tx_int (int irq, void *data, struct pt_regs *fp);
static void SCC_stat_int (int irq, void *data, struct pt_regs *fp);
static void SCC_ri_int (int irq, void *data, struct pt_regs *fp);
static int SCC_check_open( struct async_struct *info, struct tty_struct
                           *tty, struct file *file );
static void SCC_init( struct async_struct *info );
static void SCC_deinit( struct async_struct *info, int leave_dtr );
static void SCC_enab_tx_int( struct async_struct *info, int enab_flag );
static int SCC_check_custom_divisor( struct async_struct *info, int baud_base,
				    int divisor );
static void SCC_change_speed( struct async_struct *info );
static int SCC_clocksrc( unsigned baud_base, unsigned channel );
static void SCC_throttle( struct async_struct *info, int status );
static void SCC_set_break( struct async_struct *info, int break_flag );
static void SCC_get_serial_info( struct async_struct *info, struct
				serial_struct *retinfo );
static unsigned int SCC_get_modem_info( struct async_struct *info );
static int SCC_set_modem_info( struct async_struct *info, int new_dtr, int
			      new_rts );
static int SCC_ioctl( struct tty_struct *tty, struct file *file, struct
		     async_struct *info, unsigned int cmd, unsigned long
		     arg );
static void SCC_stop_receive (struct async_struct *info);
static int SCC_trans_empty (struct async_struct *info);

/************************* End of Prototypes **************************/


static SERIALSWITCH SCC_switch = {
	SCC_init, SCC_deinit, SCC_enab_tx_int,
	SCC_check_custom_divisor, SCC_change_speed,
	SCC_throttle, SCC_set_break,
	SCC_get_serial_info, SCC_get_modem_info,
	SCC_set_modem_info, SCC_ioctl, SCC_stop_receive, SCC_trans_empty,
	SCC_check_open
};


int atari_SCC_init( void )
{
	struct serial_struct req;
	int sccdma = ATARIHW_PRESENT(SCC_DMA), escc = ATARIHW_PRESENT(ST_ESCC);
	int nr = 0;
	extern char m68k_debug_device[];

	/* SCC present at all? */
	if (!MACH_IS_ATARI ||
	    !(ATARIHW_PRESENT(SCC) || ATARIHW_PRESENT(ST_ESCC)))
		return( -ENODEV );

	/* Channel A is switchable on the TT, MegaSTE and Medusa (extension), i.e.
	 * all machines with an SCC except the Falcon. If there's a machine where
	 * channel A is fixed to a RS-232 Serial2, add code to set to
	 * SCCA_SWITCH_SERIAL2_ONLY.
	 */
	if ((boot_info.bi_atari.mch_cookie >> 16) == ATARI_MCH_FALCON)
		SCC_chan_a_switchable = SCCA_SWITCH_LAN_ONLY;
	else if (ATARIHW_PRESENT(TT_MFP) ||
		 ((boot_info.bi_atari.mch_cookie >> 16) == ATARI_MCH_STE &&
		  (boot_info.bi_atari.mch_cookie & 0xffff)))
		SCC_chan_a_switchable = SCCA_SWITCH_BOTH;
	else
		SCC_chan_a_switchable = SCCA_SWITCH_SERIAL2_ONLY;

	/* General initialization */
	ChannelsReversed = escc;
	SCC_chan_a_open = 0;

	/* Init channel B */
	if (!strcmp( m68k_debug_device, "ser2" ))
		printk(KERN_NOTICE "SCC channel B: used as debug device\n" );
	else {
		req.line = DEFAULT_CHANNEL_B_LINE;
		req.type = SER_SCC_NORM;
		req.port = (int)(escc ? &st_escc.cha_b_ctrl : &scc.cha_b_ctrl);
		if ((chb_line = register_serial( &req )) >= 0) {
			SCC_init_port( &rs_table[chb_line], req.type, CHANNEL_B );
			++nr;
		}
		else
			printk(KERN_WARNING "Cannot allocate ttyS%d for SCC channel B\n", req.line );
	}

	/* Init channel A, RS232 part (Serial2) */
	if (SCC_chan_a_switchable != SCCA_SWITCH_LAN_ONLY) {
		req.line = DEFAULT_CHANNEL_A232_LINE;
		req.type = sccdma ? SER_SCC_DMA : SER_SCC_NORM;
		req.port = (int)(escc ? &st_escc.cha_a_ctrl : &scc.cha_a_ctrl);
		if ((cha232_line = register_serial( &req )) >= 0) {
			SCC_init_port( &rs_table[cha232_line], req.type, CHANNEL_A );
			++nr;
		}
		else
			printk(KERN_WARNING "Cannot allocate ttyS%d for SCC channel A\n", req.line );
	}

	/* Init channel A, RS422 part (LAN) */
	if (SCC_chan_a_switchable != SCCA_SWITCH_SERIAL2_ONLY) {
		req.line = DEFAULT_CHANNEL_A422_LINE;
		req.type = sccdma ? SER_SCC_DMA : SER_SCC_NORM;
		req.port = (int)(escc ? &st_escc.cha_a_ctrl : &scc.cha_a_ctrl);
		if ((cha422_line = register_serial( &req )) >= 0) {
			SCC_init_port( &rs_table[cha422_line], req.type, CHANNEL_A );
			++nr;
		}
		else
			printk(KERN_WARNING "Cannot allocate ttyS%d for SCC channel A\n", req.line );
	}

	return( nr > 0 ? 0 : -ENODEV );
}


static void SCC_init_port( struct async_struct *info, int type, int channel )
{
	static int called = 0, ch_a_inited = 0;

	info->sw = &SCC_switch;
	/* The SERTYPE (with or without DMA) is ignored for now... */

	/* set ISRs, but don't enable interrupts yet (done in init());
	 * All interrupts are of type PRIORITIZED, which means they can be
	 * interrupted by all level 6 ints, but not by another SCC (or other level
	 * 5) int. I see no races with any MFP int, but I'm not quite sure yet
	 * whether longer delays in between the two-stage SCC register access can
	 * break things...
	 */
	if (channel == CHANNEL_B || !ch_a_inited) {
		request_irq(channel ? IRQ_SCCB_TX : IRQ_SCCA_TX,
		            SCC_tx_int, IRQ_TYPE_PRIO,
		            channel ? "SCC-B TX" : "SCC-A TX", info);
		request_irq(channel ? IRQ_SCCB_STAT : IRQ_SCCA_STAT,
		            SCC_stat_int, IRQ_TYPE_PRIO,
		            channel ? "SCC-B status" : "SCC-A status", info);
		request_irq(channel ? IRQ_SCCB_RX : IRQ_SCCA_RX,
		            SCC_rx_int, IRQ_TYPE_PRIO,
		            channel ? "SCC-B RX" : "SCC-A RX", info);
		request_irq(channel ? IRQ_SCCB_SPCOND : IRQ_SCCA_SPCOND,
		            SCC_spcond_int, IRQ_TYPE_PRIO,
		            channel ? "SCC-B special cond" : "SCC-A special cond", info);
		if (channel != 0 && ATARIHW_PRESENT (TT_MFP))
			request_irq(IRQ_TT_MFP_RI, SCC_ri_int, IRQ_TYPE_SLOW,
			            "TT-MFP RI (modem 2)", info);
	}

	/* Hardware initialization */

	if (!called) {
		/* Before accessing the SCC the first time, do a read to the
		 * control register to reset the internal pointers
		 */
		SCCread( STATUS_REG );

		/* The master reset with additional delay */
		SCCwrite( MASTER_INT_CTRL, MIC_HARD_RESET );
		udelay(40);

		/* Set the interrupt vector, 0x60 is standard Atari */
		SCCwrite( INT_VECTOR_REG, 0x60 );

		/* Interrupt parameters: vector includes status, status low */
		SCCwrite( MASTER_INT_CTRL, MIC_VEC_INCL_STAT );

		/* to do only once, too: If a TT-MFP is present, initialize its timer
		 * C that is connected to RTxC of channel B */
		if (ATARIHW_PRESENT(TT_MFP)) {
			/* If on a TT, program the TT-MFP's timer C to 307200 kHz
			 * for RTxC on channel B
			 */
			tt_mfp.tim_ct_cd = (tt_mfp.tim_ct_cd & ~0x70) | 0x10; /* 1:4 mode */
			tt_mfp.tim_dt_c  = 1;
			/* make sure the timer interrupt is disabled, the timer is used
			 * only for generating a clock */
			atari_turnoff_irq( IRQ_TT_MFP_TIMC );

			/* Set the baud tables */
			SCC_baud_table[CHANNEL_A] = bdtab_norm;
			SCC_baud_table[CHANNEL_B] = bdtab_TTChB;

			/* The clocks are already initialzed for the TT. */
		}
		else {
			/* Set the baud tables */
			SCC_baud_table[CHANNEL_A] = bdtab_norm;
			SCC_baud_table[CHANNEL_B] = bdtab_norm;

			/* Set the clocks; only RTxCB is different compared to the TT */
			SCC_clocks[CHANNEL_B][CLK_RTxC] = SCC_BAUD_BASE_PCLK4;
		}

		SCCmod( MASTER_INT_CTRL, 0xff, MIC_MASTER_INT_ENAB );
	}

	/* disable interrupts for this channel */
	SCCwrite( INT_AND_DMA_REG, 0 );

	called = 1;
	if (CHANNR(info) == CHANNEL_A) ch_a_inited = 1;
}


#ifdef MODULE
static void SCC_deinit_port( struct async_struct *info, int channel )
{
	free_irq(channel ? IRQ_SCCB_TX : IRQ_SCCA_TX, info );
	free_irq(channel ? IRQ_SCCB_STAT : IRQ_SCCA_STAT, info );
	free_irq(channel ? IRQ_SCCB_RX : IRQ_SCCA_RX, info );
	free_irq(channel ? IRQ_SCCB_SPCOND : IRQ_SCCA_SPCOND, info );
	if (channel != 0 && ATARIHW_PRESENT (TT_MFP))
		free_irq(IRQ_TT_MFP_RI, info);
}
#endif


#if DEBUG & DEBUG_OVERRUNS
static int SCC_ch_cnt[2] = { 0, 0 }, SCC_ch_ovrrun[2] = { 0, 0 };
#endif

static void SCC_rx_int( int irq, void *data, struct pt_regs *fp)
{
	struct async_struct *info = data;
	unsigned char	ch;

	SETUP_INFO(info);

	ch = SCCread_NB( RX_DATA_REG );
#if DEBUG & DEBUG_INT
	printk( "SCC ch %d rx int: char %02x\n", CHANNR(info), ch );
#endif
	rs_receive_char (info, ch, 0);
#if DEBUG & DEBUG_OVERRUNS
	{ int channel = CHANNR(info);
	  if (++SCC_ch_cnt[channel] == 10000) {
		  printk( "SCC ch. %d: overrun rate %d.%02d\n", channel,
				  SCC_ch_ovrrun[channel] / 100,
				  SCC_ch_ovrrun[channel] % 100 );
		  SCC_ch_cnt[channel] = SCC_ch_ovrrun[channel] = 0;
	  }
	}
#endif

	/* Check if another character is already ready; in that case, the
	 * spcond_int() function must be used, because this character may have an
	 * error condition that isn't signalled by the interrupt vector used!
	 */
	if (SCCread( INT_PENDING_REG ) &
	    (CHANNR(info) == CHANNEL_A ? IPR_A_RX : IPR_B_RX)) {
		SCC_spcond_int (0, info, 0);
		return;
	}

#ifndef ATARI_USE_SOFTWARE_EOI
	SCCwrite_NB( COMMAND_REG, CR_HIGHEST_IUS_RESET );
#endif
}


static void SCC_spcond_int( int irq, void *data, struct pt_regs *fp)
{
	struct async_struct *info = data;
	unsigned char	stat, ch, err;
	int		int_pending_mask = CHANNR(info) == CHANNEL_A ?
			                   IPR_A_RX : IPR_B_RX;

	SETUP_INFO(info);

	do {

		stat = SCCread( SPCOND_STATUS_REG );
		ch = SCCread_NB(RX_DATA_REG);
#if DEBUG & DEBUG_INT
		printk( "SCC ch %d spcond int: char %02x stat %02x\n",
				CHANNR(info), ch, stat );
#endif

		if (stat & SCSR_RX_OVERRUN)
			err = TTY_OVERRUN;
		else if (stat & SCSR_PARITY_ERR)
			err = TTY_PARITY;
		else if (stat & SCSR_CRC_FRAME_ERR)
			err = TTY_FRAME;
		else
			err = 0;

		rs_receive_char (info, ch, err);

		/* Overrun and parity errors have to be cleared
		   manually, else the condition persists for the next
		   chars */
		if (stat & (SCSR_RX_OVERRUN | SCSR_PARITY_ERR))
			SCCwrite(COMMAND_REG, CR_ERROR_RESET);

#if DEBUG & DEBUG_OVERRUNS
		{ int channel = CHANNR(info);
		  if (err == TTY_OVERRUN) SCC_ch_ovrrun[channel]++;
		  if (++SCC_ch_cnt[channel] == 10000) {
			  printk( "SCC ch. %d: overrun rate %d.%02d %%\n", channel,
					  SCC_ch_ovrrun[channel] / 100,
					  SCC_ch_ovrrun[channel] % 100 );
			  SCC_ch_cnt[channel] = SCC_ch_ovrrun[channel] = 0;
		  }
	    }
#endif
	} while( SCCread( INT_PENDING_REG ) & int_pending_mask );

#ifndef ATARI_USE_SOFTWARE_EOI
	SCCwrite_NB( COMMAND_REG, CR_HIGHEST_IUS_RESET );
#endif
}

static void SCC_ri_int(int irq, void *data, struct pt_regs *fp)
{
	struct async_struct *info = data;
	/* update input line counter */
	info->icount.rng++;
	wake_up_interruptible(&info->delta_msr_wait);
}

static void SCC_tx_int( int irq, void *data, struct pt_regs *fp)
{
	struct async_struct *info = data;
	int ch;

	SETUP_INFO(info);

	while( (SCCread_NB( STATUS_REG ) & SR_TX_BUF_EMPTY) &&
		   (ch = rs_get_tx_char( info )) >= 0 ) {
		SCCwrite( TX_DATA_REG, ch );
#if DEBUG & DEBUG_INT
		printk( "SCC ch. %d tx int: sent char %02x\n", CHANNR(info), ch );
#endif
	}
	if (rs_no_more_tx( info )) {
		SCCwrite( COMMAND_REG, CR_TX_PENDING_RESET );
		/* disable tx interrupts */
		SCCmod (INT_AND_DMA_REG, ~IDR_TX_INT_ENAB, 0);
#if DEBUG & DEBUG_INT
		printk ("SCC ch %d tx int: no more chars\n",
				CHANNR (info));
#endif
	}

#ifndef ATARI_USE_SOFTWARE_EOI
	SCCwrite_NB( COMMAND_REG, CR_HIGHEST_IUS_RESET );
#endif
}


static void SCC_stat_int( int irq, void *data, struct pt_regs *fp)
{
	struct async_struct *info = data;
	unsigned channel = CHANNR(info);
	unsigned char	last_sr, sr, changed;

	SETUP_INFO(info);

	last_sr = SCC_last_status_reg[channel];
	sr = SCC_last_status_reg[channel] = SCCread_NB( STATUS_REG );
	changed = last_sr ^ sr;
#if DEBUG & DEBUG_INT
	printk( "SCC ch %d stat int: sr=%02x last_sr=%02x\n",
			CHANNR(info), sr, last_sr );
#endif

	if (changed & SR_DCD)
		rs_dcd_changed( info, sr & SR_DCD );

	if (changed & SR_CTS) {
#if DEBUG & DEBUG_THROTTLE
		printk( "SCC ch. %d: now CTS=%d\n", CHANNR(info), !!(sr & SR_CTS) );
#endif
		rs_check_cts( info, sr & SR_CTS );
	}

	if (changed & SR_SYNC_ABORT) { /* Data Set Ready */
		/* update input line counter */
		info->icount.dsr++;
		wake_up_interruptible(&info->delta_msr_wait);
	}

	SCCwrite( COMMAND_REG, CR_EXTSTAT_RESET );
#ifndef ATARI_USE_SOFTWARE_EOI
	SCCwrite_NB( COMMAND_REG, CR_HIGHEST_IUS_RESET );
#endif
}


static int SCC_check_open( struct async_struct *info, struct tty_struct *tty,
			  struct file *file )
{
	unsigned long flags;
	unsigned char tmp;

	/* If channel A is opened, check if one of the compounded ports (ttyS3 and
	 * ttyS4) is already open, else activate the appropriate port hardware.
	 */

#if DEBUG & DEBUG_OPEN
	printk( "SCC: about to open channel %d as line %d\n",
			CHANNR(info), info->line );
#endif

	if (CHANNR(info) == CHANNEL_A) {

		if (SCC_chan_a_open) {
			if (SCC_chan_a_line != info->line) {
#if DEBUG & DEBUG_OPEN
				printk("SCC: channel 0 was already open\n");
#endif
				return -EBUSY;
			}
			else
				return 0;
		}

		if ((info->line == cha232_line &&
		     SCC_chan_a_switchable == SCCA_SWITCH_LAN_ONLY) ||
		    (info->line == cha422_line &&
		     SCC_chan_a_switchable == SCCA_SWITCH_SERIAL2_ONLY))
			return( -ENODEV );

		SCC_chan_a_open = 1;
		SCC_chan_a_line = info->line;

		if (SCC_chan_a_switchable == SCCA_SWITCH_BOTH) {
			save_flags(flags);
			cli();
			sound_ym.rd_data_reg_sel = 14;
			tmp = sound_ym.rd_data_reg_sel;
			sound_ym.wd_data = (info->line == cha232_line
					    ? tmp | 0x80
					    : tmp & 0x7f);
#if DEBUG & DEBUG_OPEN
			printk( "SCC: set PSG IO7 to %02x (was %02x)\n",
			       (info->line & 1) ? (tmp | 0x80) : (tmp & 0x7f),
			       tmp );
#endif
			restore_flags(flags);
		}

	}
	return( 0 );
}


static void SCC_init( struct async_struct *info )
{
	int i, channel = CHANNR(info);
	unsigned long	flags;
	static const struct {
		unsigned reg, val;
	} init_tab[] = {
		/* no parity, 1 stop bit, async, 1:16 */
		{ AUX1_CTRL_REG, A1CR_PARITY_NONE|A1CR_MODE_ASYNC_1|A1CR_CLKMODE_x64 },
		/* parity error is special cond, ints disabled, no DMA */
		{ INT_AND_DMA_REG, IDR_PARERR_AS_SPCOND | IDR_RX_INT_DISAB },
		/* Rx 8 bits/char, no auto enable, Rx off */
		{ RX_CTRL_REG, RCR_CHSIZE_8 },
		/* DTR off, Tx 8 bits/char, RTS off, Tx off */
		{ TX_CTRL_REG, TCR_CHSIZE_8 },
		/* special features off */
		{ AUX2_CTRL_REG, 0 },
		/* RTxC is XTAL, TRxC is input, both clocks = RTxC */
		{ CLK_CTRL_REG, CCR_TRxCOUT_XTAL | CCR_TXCLK_RTxC | CCR_RXCLK_RTxC },
		{ DPLL_CTRL_REG, 0 },
		/* Start Rx */
		{ RX_CTRL_REG, RCR_RX_ENAB | RCR_CHSIZE_8 },
		/* Start Tx */
		{ TX_CTRL_REG, TCR_TX_ENAB | TCR_RTS | TCR_DTR | TCR_CHSIZE_8 },
		/* Ext/Stat ints: CTS, DCD, SYNC (DSR) */
		{ INT_CTRL_REG, ICR_ENAB_DCD_INT | ICR_ENAB_CTS_INT | ICR_ENAB_SYNC_INT },
		/* Reset Ext/Stat ints */
		{ COMMAND_REG, CR_EXTSTAT_RESET },
		/* ...again */
		{ COMMAND_REG, CR_EXTSTAT_RESET },
		/* Rx int always, TX int off, Ext/Stat int on */
		{ INT_AND_DMA_REG, IDR_EXTSTAT_INT_ENAB |
		  IDR_PARERR_AS_SPCOND | IDR_RX_INT_ALL }
	};

	save_flags(flags);
	cli();

	SCCmod( MASTER_INT_CTRL, 0x3f,
	       channel == 0 ? MIC_CH_A_RESET : MIC_CH_B_RESET );
	udelay(40); /* extra delay after a reset */

	for( i = 0; i < sizeof(init_tab)/sizeof(*init_tab); ++i )
		SCCwrite( init_tab[i].reg, init_tab[i].val );

	/* remember status register for detection of DCD and CTS changes */
	SCC_last_status_reg[channel] = SCCread( STATUS_REG );
	restore_flags(flags);
#if DEBUG & DEBUG_INIT
	printk( "SCC channel %d inited\n", CHANNR(info) );
#endif
#if DEBUG & DEBUG_OPEN
	printk( "SCC channel %d opened\n", CHANNR(info) );
#endif
	MOD_INC_USE_COUNT;
}


static void SCC_deinit( struct async_struct *info, int leave_dtr )
{
	unsigned long	flags, timeout;

#if DEBUG & DEBUG_INIT
	printk( "SCC channel %d about to be deinited\n", CHANNR(info) );
#endif
	save_flags(flags);
	cli();

	/* disable interrupts */
	SCCmod( INT_AND_DMA_REG, ~(IDR_EXTSTAT_INT_ENAB | IDR_TX_INT_ENAB |
							   IDR_RX_INT_SPCOND), 0 );

	/* disable Rx */
	SCCmod( RX_CTRL_REG, ~RCR_RX_ENAB, 0 );

	/* disable Transmitter */
	SCCmod( TX_CTRL_REG, ~TCR_TX_ENAB, 0 );

	/* wait until character is completely sent */
	timeout = jiffies + 50;
	restore_flags(flags);
	while( !(SCCread( SPCOND_STATUS_REG ) & SCSR_ALL_SENT) ) {
		if (jiffies > timeout) break;
	}
	save_flags(flags);
	cli();

	/* drop RTS and maybe DTR */
	SCCmod( TX_CTRL_REG, ~(TCR_RTS | (leave_dtr ? 0 : TCR_DTR)), 0 );

	restore_flags(flags);
#if DEBUG & DEBUG_INIT
	printk( "SCC channel %d deinited\n", CHANNR(info) );
#endif

	if (CHANNR(info) == CHANNEL_A)
		SCC_chan_a_open = 0;
#if DEBUG & DEBUG_OPEN
	printk( "SCC channel %d closed, chanAlock now = %d\n",
			CHANNR(info), SCC_chan_a_open );
#endif
	MOD_DEC_USE_COUNT;
}


static void SCC_enab_tx_int( struct async_struct *info, int enab_flag )
{
	unsigned long	flags;
	unsigned char	iadr;

	save_flags(flags);
	cli();

	iadr = SCCread( INT_AND_DMA_REG );
	if (!!(iadr & IDR_TX_INT_ENAB) != !!enab_flag) {
		SCCwrite(INT_AND_DMA_REG, iadr ^ IDR_TX_INT_ENAB);
		if (enab_flag)
			/* restart the transmitter */
			SCC_tx_int (0, info, 0);
#if DEBUG & DEBUG_INT
		printk("SCC channel %d: tx int %sabled\n", CHANNR(info),
		       enab_flag ? "en" : "dis");
#endif
	}

	restore_flags(flags);
}


static int SCC_check_custom_divisor( struct async_struct *info,
				    int baud_base, int divisor )
{
	int		clksrc;

	clksrc = SCC_clocksrc (baud_base, CHANNR (info));
	if (clksrc < 0)
		/* invalid baud base */
		return( -1 );

	/* check for maximum (BRG values start from 4 with step 2) */
	if (divisor/2-2 > 65535)
		return( -1 );

	switch( clksrc ) {

	  case CLK_PCLK:
		/* The master clock can only be used with the BRG, divisors
		 * range from 4 and must be a multiple of 2
		 */
		return( !(divisor >= 4 && (divisor & 1) == 0) );

	  case CLK_RTxC:
		/* The RTxC clock can either be used for the direct 1:16, 1:32
		 * or 1:64 modes (divisors 1, 2 or 4, resp.) or with the BRG
		 * (divisors from 4 and a multiple of 2)
		 */
		return( !(divisor >= 1 && (divisor == 1 || (divisor & 1) == 0)) );

	  case CLK_TRxC:
		/* The TRxC clock can only be used for direct 1:16, 1:32 or
		 * 1:64 modes
		 */
		return( !(divisor == 1 || divisor == 2 || divisor == 4) );

	}
	return( -1 );
}


static void SCC_change_speed( struct async_struct *info )
{
	unsigned		cflag, baud, chsize, aflags;
	unsigned		channel, div = 0, clkmode, brgmode, brgval;
	int clksrc = 0;
	unsigned long	flags;

	if (!info->tty || !info->tty->termios) return;

	cflag  = info->tty->termios->c_cflag;
	baud   = cflag & CBAUD;
	chsize = (cflag & CSIZE) >> 4;
	aflags = info->flags & ASYNC_SPD_MASK;

	if (cflag & CRTSCTS)
		info->flags |= ASYNC_CTS_FLOW;
	else
		info->flags &= ~ASYNC_CTS_FLOW;
	if (cflag & CLOCAL)
		info->flags &= ~ASYNC_CHECK_CD;
	else
		info->flags |= ASYNC_CHECK_CD;

	channel = CHANNR(info);

#if DEBUG & DEBUG_SPEED
	printk( "SCC channel %d: doing new settings:\n", CHANNR(info) );
	printk( "  baud=%d chsize=%d aflags=%04x base_baud=%d divisor=%d\n",
			baud, chsize, aflags, info->baud_base, info->custom_divisor );
#endif

	if (baud == 0 && !aflags) {
		/* speed == 0 -> drop DTR */
		save_flags(flags);
		cli();
		SCCmod( TX_CTRL_REG, ~TCR_DTR, 0 );
		restore_flags(flags);
		return;
	}

	if (baud & CBAUDEX) {
		baud &= ~CBAUDEX;
		if (baud < 1 || baud > 2)
			info->tty->termios->c_cflag &= ~CBAUDEX;
		else
			baud += 15;
	}
	if (baud == 15 && aflags) {
		switch( aflags) {
		  case ASYNC_SPD_HI:
			baud = 16;
			break;
		  case ASYNC_SPD_VHI:
			baud = 17;
			break;
		  case ASYNC_SPD_CUST:
			/* Custom divisor: Compute clock source from the base_baud
			 * field */
			if ((clksrc = SCC_clocksrc( info->baud_base, channel )) < 0)
				/* This shouldn't happen... the baud_base has been checked
				 * before by check_custom_divisor() */
				return;
			div = info->custom_divisor;
		}
	}

	if (!div) {
		if (baud > 17) baud = 17;
		clksrc = SCC_baud_table[channel][baud].clksrc;
		div = SCC_baud_table[channel][baud].div;
	}

	/* compute the SCC's clock source, clock mode, BRG mode and BRG
	 * value from clksrc and div
	 */
	if (div <= 4) {
		clkmode = (div == 1 ? A1CR_CLKMODE_x16 :
			   div == 2 ? A1CR_CLKMODE_x32 :
				      A1CR_CLKMODE_x64);
		clksrc  = (clksrc == CLK_RTxC
			   ? CCR_TXCLK_RTxC | CCR_RXCLK_RTxC
			   : CCR_TXCLK_TRxC | CCR_RXCLK_TRxC);
		brgmode = 0; /* off */
		brgval  = 0;
	}
	else {
		brgval  = div/2 - 2;
		brgmode = (DCR_BRG_ENAB |
			   (clksrc == CLK_PCLK ? DCR_BRG_USE_PCLK : 0));
		clkmode = A1CR_CLKMODE_x16;
		clksrc  = CCR_TXCLK_BRG | CCR_RXCLK_BRG;
	}

	/* Now we have all parameters and can go to set them: */
	save_flags(flags);
	cli();
#if DEBUG & DEBUG_SPEED
	printk( "  brgval=%d brgmode=%02x clkmode=%02x clksrc=%02x\n",
			brgval, brgmode, clkmode, clksrc );
#endif

	/* receiver's character size */
	SCCmod( RX_CTRL_REG, ~RCR_CHSIZE_MASK, chsize << 6 );
#if DEBUG & DEBUG_SPEED
	printk( "  RX_CTRL_REG <- %02x\n", SCCread( RX_CTRL_REG ) );
#endif

	/* parity and stop bits (both, Tx and Rx) and clock mode */
	SCCmod (AUX1_CTRL_REG,
		~(A1CR_PARITY_MASK | A1CR_MODE_MASK | A1CR_CLKMODE_MASK),
		((cflag & PARENB
		  ? (cflag & PARODD ? A1CR_PARITY_ODD : A1CR_PARITY_EVEN)
		  : A1CR_PARITY_NONE)
		 | (cflag & CSTOPB ? A1CR_MODE_ASYNC_2 : A1CR_MODE_ASYNC_1)
		 | clkmode));
#if DEBUG & DEBUG_SPEED
	printk( "  AUX1_CTRL_REG <- %02x\n", SCCread( AUX1_CTRL_REG ) );
#endif

	/* sender's character size */
	/* Set DTR for valid baud rates! Tnx to jds@kom.auc.dk */
	SCCmod( TX_CTRL_REG, ~TCR_CHSIZE_MASK, chsize << 5 | TCR_DTR );
#if DEBUG & DEBUG_SPEED
	printk( "  TX_CTRL_REG <- %02x\n", SCCread( TX_CTRL_REG ) );
#endif

	/* clock sources */
	SCCmod( CLK_CTRL_REG, ~(CCR_TXCLK_MASK | CCR_RXCLK_MASK), clksrc );
#if DEBUG & DEBUG_SPEED
	printk( "  CLK_CTRL_REG <- %02x\n", SCCread( CLK_CTRL_REG ) );
#endif

	/* disable BRG before changing the value */
	SCCmod( DPLL_CTRL_REG, ~DCR_BRG_ENAB, 0 );

	/* BRG value */
	SCCwrite( TIMER_LOW_REG, brgval & 0xff );
	SCCwrite( TIMER_HIGH_REG, (brgval >> 8) & 0xff );
#if DEBUG & DEBUG_SPEED
	printk( "  TIMER_LOW_REG <- %02x\n", SCCread( TIMER_LOW_REG ) );
	printk( "  TIMER_HIGH_REG <- %02x\n", SCCread( TIMER_HIGH_REG ) );
#endif

	/* BRG enable and clock source */
	SCCmod( DPLL_CTRL_REG, ~(DCR_BRG_ENAB | DCR_BRG_USE_PCLK), brgmode );
#if DEBUG & DEBUG_SPEED
	printk( "  DPLL_CTRL_REG <- %02x\n", SCCread( DPLL_CTRL_REG ) );
#endif

	restore_flags(flags);
}


static int SCC_clocksrc( unsigned baud_base, unsigned channel )
{
	if (baud_base == SCC_PCLK)
		return( CLK_PCLK );
	else if (SCC_clocks[channel][CLK_RTxC] != SCC_BAUD_BASE_NONE &&
		 baud_base == SCC_clocks[channel][CLK_RTxC])
		return( CLK_RTxC );
	else if (SCC_clocks[channel][CLK_TRxC] != SCC_BAUD_BASE_NONE &&
		 baud_base == SCC_clocks[channel][CLK_TRxC])
		return( CLK_TRxC );
	else
		return( -1 );
}

static void SCC_throttle( struct async_struct *info, int status )
{
	unsigned long	flags;

#if DEBUG & DEBUG_THROTTLE
	printk( "SCC channel %d: throttle %s\n",
	       CHANNR(info), status ? "full" : "avail" );
#endif
	save_flags(flags);
	cli();

	if (status)
		SCCmod( TX_CTRL_REG, ~TCR_RTS, 0 );
	else
		SCCmod( TX_CTRL_REG, 0xff, TCR_RTS );

#if DEBUG & DEBUG_THROTTLE
	printk( "  now TX_CTRL_REG = %02x\n", SCCread( TX_CTRL_REG ) );
#endif

	restore_flags(flags);
}


static void SCC_set_break( struct async_struct *info, int break_flag )
{
	unsigned long	flags;

	save_flags(flags);
	cli();

	if (break_flag) {
		SCCmod( TX_CTRL_REG, 0xff, TCR_SEND_BREAK );
	} else {
		SCCmod( TX_CTRL_REG, ~TCR_SEND_BREAK, 0 );
	}

	restore_flags(flags);
}


static void SCC_get_serial_info( struct async_struct *info,
				struct serial_struct *retinfo )
{
	retinfo->baud_base = info->baud_base;
	retinfo->custom_divisor = info->custom_divisor;
}


static unsigned int SCC_get_modem_info( struct async_struct *info )
{
	unsigned	sr, tcr, ri, dsr;
	unsigned long	flags;

	save_flags(flags);
	cli();
	sr = SCCread( STATUS_REG );
	tcr = SCCread( TX_CTRL_REG );
	restore_flags(flags);
#if DEBUG & DEBUG_INFO
	printk( "SCC channel %d: get info, sr=%02x tcr=%02x\n",
			CHANNR(info), sr, tcr );
#endif
	if (CHANNR (info) == 0)
		ri = 0;
	else if (ATARIHW_PRESENT (TT_MFP))
		ri = tt_mfp.par_dt_reg & (1 << 3) ? 0 : TIOCM_RNG;
	else
		ri = mfp.par_dt_reg & (1 << 6) ? 0 : TIOCM_RNG;

	if (ATARIHW_PRESENT (ST_ESCC))
		dsr = st_escc_dsr & (1 << (3 - CHANNR(info))) ? TIOCM_DSR : 0;
	else
		dsr = sr & SR_SYNC_ABORT ? TIOCM_DSR : 0;

	return (((tcr & TCR_RTS) ? TIOCM_RTS : 0) |
		((tcr & TCR_DTR) ? TIOCM_DTR : 0) |
		((sr & SR_DCD ) ? TIOCM_CAR : 0) |
		((sr & SR_CTS ) ? TIOCM_CTS : 0) |
		dsr | ri);
}


static int SCC_set_modem_info( struct async_struct *info,
			      int new_dtr, int new_rts )
{
	unsigned long	flags;

	save_flags(flags);
	cli();

	if (new_dtr == 0) {
		SCCmod( TX_CTRL_REG, ~TCR_DTR, 0 );
	} else if (new_dtr == 1) {
		SCCmod( TX_CTRL_REG, 0xff, TCR_DTR );
	}

	if (new_rts == 0) {
		SCCmod( TX_CTRL_REG, ~TCR_RTS, 0 );
	} else if (new_rts == 1) {
		SCCmod( TX_CTRL_REG, 0xff, TCR_RTS );
	}

#if DEBUG & DEBUG_INFO
	printk( "SCC channel %d: set info (dtr=%d,rts=%d), now tcr=%02x\n",
	       CHANNR(info), new_dtr, new_rts, SCCread( TX_CTRL_REG ) );
#endif

	restore_flags(flags);
	return( 0 );
}

static void SCC_stop_receive (struct async_struct *info)
{
	/* disable Rx interrupts */
	SCCmod (INT_AND_DMA_REG, ~IDR_RX_INT_MASK, 0);

	/* disable Rx */
	SCCmod (RX_CTRL_REG, ~RCR_RX_ENAB, 0);
}

static int SCC_trans_empty (struct async_struct *info)
{
	return (SCCread (SPCOND_STATUS_REG) & SCSR_ALL_SENT) != 0;
}

static int SCC_ioctl( struct tty_struct *tty, struct file *file,
		     struct async_struct *info, unsigned int cmd,
		     unsigned long arg )
{
	struct atari_SCCserial *cp = (void *)arg;
	int error;
	unsigned channel = CHANNR(info), i, clk, div, rtxc, trxc, pclk;

	switch( cmd ) {

	  case TIOCGATSCC:

		error = verify_area( VERIFY_WRITE, (void *)arg,
				    sizeof(struct atari_SCCserial) );
		if (error)
			return error;

		put_fs_long(SCC_clocks[channel][CLK_RTxC], &cp->RTxC_base);
		put_fs_long(SCC_clocks[channel][CLK_TRxC], &cp->TRxC_base);
		put_fs_long(SCC_PCLK, &cp->PCLK_base);
		memcpy_tofs( cp->baud_table, SCC_baud_table[channel] + 1,
			    sizeof(cp->baud_table) );

		return( 0 );

	  case TIOCSATSCC:

		if (!suser()) return( -EPERM );

		error = verify_area(VERIFY_READ, (void *)arg,
				    sizeof(struct atari_SCCserial) );
		if (error)
			return error;

		rtxc = get_fs_long( (unsigned long *)&(cp->RTxC_base) );
		trxc = get_fs_long( (unsigned long *)&(cp->TRxC_base) );
		pclk = get_fs_long( (unsigned long *)&(cp->PCLK_base) );

		if (pclk == SCC_BAUD_BASE_NONE)
			/* This is really not possible :-) */
			return( -EINVAL );

		/* Check the baud table for consistency */
		for( i = 0; i < sizeof(cp->baud_table)/sizeof(cp->baud_table[0]); ++i ) {

			clk = get_fs_long( (unsigned long *)&(cp->baud_table[i].clksrc) );
			div = get_fs_long( (unsigned long *)&(cp->baud_table[i].divisor) );

			switch( clk ) {
			  case CLK_RTxC:
				if (rtxc == SCC_BAUD_BASE_NONE)
					return( -EINVAL );
				if (((div & 1) && div != 1) ||
				    (div >= 4 && div/2-2 > 65535))
					return( -EINVAL );
				break;
			  case CLK_TRxC:
				if (trxc == SCC_BAUD_BASE_NONE)
					return( -EINVAL );
				if (div != 1 && div != 2 && div != 4)
					return( -EINVAL );
				break;
			  case CLK_PCLK:
				if (div < 4 || (div & 1) || div/2-2 > 65535)
					return( -EINVAL );
				break;
			  default:
				/* invalid valid clock source */
				return( -EINVAL );
			}
		}

		/* After all the checks, set the values */

		SCC_clocks[channel][CLK_RTxC] = rtxc;
		SCC_clocks[channel][CLK_TRxC] = trxc;
		SCC_PCLK = pclk;

		memcpy_fromfs( bdtab_usr[channel] + 1, cp->baud_table,
			      sizeof(cp->baud_table) );
		/* Now use the user supplied baud table */
		SCC_baud_table[channel] = bdtab_usr[channel];

		return( 0 );

	  case TIOCDATSCC:

		if (!suser()) return( -EPERM );

		if (ATARIHW_PRESENT(TT_MFP)) {
			SCC_clocks[channel][CLK_RTxC] =
				(channel == CHANNEL_A) ?
					SCC_BAUD_BASE_PCLK4 :
					SCC_BAUD_BASE_TIMC;
			SCC_clocks[channel][CLK_TRxC] =
				(channel == CHANNEL_A) ?
					SCC_BAUD_BASE_NONE :
					SCC_BAUD_BASE_BCLK;
		}
		else {
			SCC_clocks[channel][CLK_RTxC] = SCC_BAUD_BASE_PCLK4;
			SCC_clocks[channel][CLK_TRxC] =
				(channel == CHANNEL_A) ?
					SCC_BAUD_BASE_NONE :
					SCC_BAUD_BASE_BCLK;
		}

		SCC_PCLK = SCC_BAUD_BASE_PCLK;
		SCC_baud_table[channel] =
			((ATARIHW_PRESENT(TT_MFP) && channel == 1) ?
			 bdtab_TTChB : bdtab_norm);

		return( 0 );

	}
	return( -ENOIOCTLCMD );
}




#ifdef MODULE
int init_module(void)
{
	return( atari_SCC_init() );
}

void cleanup_module(void)
{
	if (chb_line >= 0) {
		SCC_deinit_port( &rs_table[chb_line], CHANNEL_B );
		unregister_serial( chb_line );
	}
	if (cha232_line >= 0 || cha422_line >= 0)
		SCC_deinit_port( &rs_table[cha232_line], CHANNEL_A );
	if (cha232_line >= 0)
		unregister_serial( cha232_line );
	if (cha422_line >= 0)
		unregister_serial( cha422_line );
}
#endif

/*
 * Local variables:
 *  c-indent-level: 4
 *  tab-width: 4
 * End:
 */
