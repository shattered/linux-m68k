/*
 * atari_SCC.h: Definitions for the Am8530 Serial Communications Controller
 *
 * Copyright 1994 Roman Hodek <Roman.Hodek@informatik.uni-erlangen.de>
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file COPYING in the main directory of this archive
 * for more details.
 *
 */


#ifndef _ATARI_SCC_H
#define _ATARI_SCC_H

#include <asm/atarihw.h>

/***********************************************************************/
/*                                                                     */
/*                             Register Names                          */
/*                                                                     */
/***********************************************************************/

/* The SCC documentation gives no explicit names to the registers,
 * they're just called WR0..15 and RR0..15. To make the source code
 * better readable and make the transparent write reg read access (see
 * below) possible, I christen them here with self-invented names.
 * Note that (real) read registers are assigned numbers 16..31. WR7'
 * has number 33.
 */

#define	COMMAND_REG			0	/* wo */
#define	INT_AND_DMA_REG		1	/* wo */
#define	INT_VECTOR_REG		2	/* rw, common to both channels */
#define	RX_CTRL_REG			3	/* rw */
#define	AUX1_CTRL_REG		4	/* rw */
#define	TX_CTRL_REG			5	/* rw */
#define	SYNC_ADR_REG		6	/* wo */
#define	SYNC_CHAR_REG		7	/* wo */
#define	SDLC_OPTION_REG		33	/* wo */
#define	TX_DATA_REG			8	/* wo */
#define	MASTER_INT_CTRL		9	/* wo, common to both channels */
#define	AUX2_CTRL_REG		10	/* rw */
#define	CLK_CTRL_REG		11	/* wo */
#define	TIMER_LOW_REG		12	/* rw */
#define	TIMER_HIGH_REG		13	/* rw */
#define	DPLL_CTRL_REG		14	/* wo */
#define	INT_CTRL_REG		15	/* rw */

#define	STATUS_REG			16	/* ro */
#define	SPCOND_STATUS_REG	17	/* wo */
/* RR2 is WR2 for Channel A, Channel B gives vector + current status: */
#define	CURR_VECTOR_REG		18	/* Ch. B only, Ch. A for rw */
#define	INT_PENDING_REG		19	/* Channel A only! */
/* RR4 is WR4, if b6(MR7') == 1 */
/* RR5 is WR5, if b6(MR7') == 1 */
#define	FS_FIFO_LOW_REG		22	/* ro */
#define	FS_FIFO_HIGH_REG	23	/* ro */
#define	RX_DATA_REG			24	/* ro */
/* RR9 is WR3, if b6(MR7') == 1 */
#define	DPLL_STATUS_REG		26	/* ro */
/* RR11 is WR10, if b6(MR7') == 1 */
/* RR12 is WR12 */
/* RR13 is WR13 */
/* RR14 not present */
/* RR15 is WR15 */


/***********************************************************************/
/*                                                                     */
/*                             Register Values                         */
/*                                                                     */
/***********************************************************************/


/* WR0: COMMAND_REG "CR" */

#define	CR_RX_CRC_RESET			0x40
#define	CR_TX_CRC_RESET			0x80
#define	CR_TX_UNDERRUN_RESET	0xc0

#define	CR_EXTSTAT_RESET		0x10
#define	CR_SEND_ABORT			0x18
#define	CR_ENAB_INT_NEXT_RX		0x20
#define	CR_TX_PENDING_RESET		0x28
#define	CR_ERROR_RESET			0x30
#define	CR_HIGHEST_IUS_RESET	0x38


/* WR1: INT_AND_DMA_REG "IDR" */

#define	IDR_EXTSTAT_INT_ENAB	0x01
#define	IDR_TX_INT_ENAB			0x02
#define	IDR_PARERR_AS_SPCOND	0x04

#define	IDR_RX_INT_DISAB		0x00
#define	IDR_RX_INT_FIRST		0x08
#define	IDR_RX_INT_ALL			0x10
#define	IDR_RX_INT_SPCOND		0x18
#define	IDR_RX_INT_MASK			0x18

#define	IDR_WAITREQ_RX			0x20
#define	IDR_WAITREQ_IS_REQ		0x40
#define	IDR_WAITREQ_ENAB		0x80


/* WR3: RX_CTRL_REG "RCR" */

#define	RCR_RX_ENAB				0x01
#define	RCR_DISCARD_SYNC_CHARS	0x02
#define	RCR_ADDR_SEARCH			0x04
#define	RCR_CRC_ENAB			0x08
#define	RCR_SEARCH_MODE			0x10
#define	RCR_AUTO_ENAB_MODE		0x20

#define	RCR_CHSIZE_MASK			0xc0
#define	RCR_CHSIZE_5			0x00
#define	RCR_CHSIZE_6			0x40
#define	RCR_CHSIZE_7			0x80
#define	RCR_CHSIZE_8			0xc0


/* WR4: AUX1_CTRL_REG "A1CR" */

#define	A1CR_PARITY_MASK		0x03
#define	A1CR_PARITY_NONE		0x00
#define	A1CR_PARITY_ODD			0x01
#define	A1CR_PARITY_EVEN		0x03

#define	A1CR_MODE_MASK			0x0c
#define	A1CR_MODE_SYNCR			0x00
#define	A1CR_MODE_ASYNC_1		0x04
#define	A1CR_MODE_ASYNC_15		0x08
#define	A1CR_MODE_ASYNC_2		0x0c

#define	A1CR_SYNCR_MODE_MASK	0x30
#define	A1CR_SYNCR_MONOSYNC		0x00
#define	A1CR_SYNCR_BISYNC		0x10
#define	A1CR_SYNCR_SDLC			0x20
#define	A1CR_SYNCR_EXTCSYNC		0x30

#define	A1CR_CLKMODE_MASK		0xc0
#define	A1CR_CLKMODE_x1			0x00
#define	A1CR_CLKMODE_x16		0x40
#define	A1CR_CLKMODE_x32		0x80
#define	A1CR_CLKMODE_x64		0xc0


/* WR5: TX_CTRL_REG "TCR" */

#define	TCR_TX_CRC_ENAB			0x01
#define	TCR_RTS					0x02
#define	TCR_USE_CRC_CCITT		0x00
#define	TCR_USE_CRC_16			0x04
#define	TCR_TX_ENAB				0x08
#define	TCR_SEND_BREAK			0x10

#define	TCR_CHSIZE_MASK			0x60
#define	TCR_CHSIZE_5			0x00
#define	TCR_CHSIZE_6			0x20
#define	TCR_CHSIZE_7			0x40
#define	TCR_CHSIZE_8			0x60

#define	TCR_DTR					0x80


/* WR7': SLDC_OPTION_REG "SOR" */

#define	SOR_AUTO_TX_ENAB		0x01
#define	SOR_AUTO_EOM_RESET		0x02
#define	SOR_AUTO_RTS_MODE		0x04
#define	SOR_NRZI_DISAB_HIGH		0x08
#define	SOR_ALT_DTRREQ_TIMING	0x10
#define	SOR_READ_CRC_CHARS		0x20
#define	SOR_EXTENDED_REG_ACCESS	0x40


/* WR9: MASTER_INT_CTRL "MIC" */

#define	MIC_VEC_INCL_STAT		0x01
#define	MIC_NO_VECTOR			0x02
#define	MIC_DISAB_LOWER_CHAIN	0x04
#define	MIC_MASTER_INT_ENAB		0x08
#define	MIC_STATUS_HIGH			0x10
#define	MIC_IGN_INTACK			0x20

#define	MIC_NO_RESET			0x00
#define	MIC_CH_A_RESET			0x40
#define	MIC_CH_B_RESET			0x80
#define	MIC_HARD_RESET			0xc0


/* WR10: AUX2_CTRL_REG "A2CR" */

#define	A2CR_SYNC_6				0x01
#define	A2CR_LOOP_MODE			0x02
#define	A2CR_ABORT_ON_UNDERRUN	0x04
#define	A2CR_MARK_IDLE			0x08
#define	A2CR_GO_ACTIVE_ON_POLL	0x10

#define	A2CR_CODING_MASK		0x60
#define	A2CR_CODING_NRZ			0x00
#define	A2CR_CODING_NRZI		0x20
#define	A2CR_CODING_FM1			0x40
#define	A2CR_CODING_FM0			0x60

#define	A2CR_PRESET_CRC_1		0x80


/* WR11: CLK_CTRL_REG "CCR" */

#define	CCR_TRxCOUT_MASK		0x03
#define	CCR_TRxCOUT_XTAL		0x00
#define	CCR_TRxCOUT_TXCLK		0x01
#define	CCR_TRxCOUT_BRG			0x02
#define	CCR_TRxCOUT_DPLL		0x03

#define	CCR_TRxC_OUTPUT			0x04

#define	CCR_TXCLK_MASK			0x18
#define	CCR_TXCLK_RTxC			0x00
#define	CCR_TXCLK_TRxC			0x08
#define	CCR_TXCLK_BRG			0x10
#define	CCR_TXCLK_DPLL			0x18

#define	CCR_RXCLK_MASK			0x60
#define	CCR_RXCLK_RTxC			0x00
#define	CCR_RXCLK_TRxC			0x20
#define	CCR_RXCLK_BRG			0x40
#define	CCR_RXCLK_DPLL			0x60

#define	CCR_RTxC_XTAL			0x80


/* WR14: DPLL_CTRL_REG "DCR" */

#define	DCR_BRG_ENAB			0x01
#define	DCR_BRG_USE_PCLK		0x02
#define	DCR_DTRREQ_IS_REQ		0x04
#define	DCR_AUTO_ECHO			0x08
#define	DCR_LOCAL_LOOPBACK		0x10

#define	DCR_DPLL_EDGE_SEARCH	0x20
#define	DCR_DPLL_ERR_RESET		0x40
#define	DCR_DPLL_DISAB			0x60
#define	DCR_DPLL_CLK_BRG		0x80
#define	DCR_DPLL_CLK_RTxC		0xa0
#define	DCR_DPLL_FM				0xc0
#define	DCR_DPLL_NRZI			0xe0


/* WR15: INT_CTRL_REG "ICR" */

#define	ICR_OPTIONREG_SELECT	0x01
#define	ICR_ENAB_BRG_ZERO_INT	0x02
#define	ICR_USE_FS_FIFO			0x04
#define	ICR_ENAB_DCD_INT		0x08
#define	ICR_ENAB_SYNC_INT		0x10
#define	ICR_ENAB_CTS_INT		0x20
#define	ICR_ENAB_UNDERRUN_INT	0x40
#define	ICR_ENAB_BREAK_INT		0x80


/* RR0: STATUS_REG "SR" */

#define	SR_CHAR_AVAIL			0x01
#define	SR_BRG_ZERO				0x02
#define	SR_TX_BUF_EMPTY			0x04
#define	SR_DCD					0x08
#define	SR_SYNC_ABORT			0x10
#define	SR_CTS					0x20
#define	SR_TX_UNDERRUN			0x40
#define	SR_BREAK				0x80


/* RR1: SPCOND_STATUS_REG "SCSR" */

#define	SCSR_ALL_SENT			0x01
#define	SCSR_RESIDUAL_MASK		0x0e
#define	SCSR_PARITY_ERR			0x10
#define	SCSR_RX_OVERRUN			0x20
#define	SCSR_CRC_FRAME_ERR		0x40
#define	SCSR_END_OF_FRAME		0x80


/* RR3: INT_PENDING_REG "IPR" */

#define	IPR_B_EXTSTAT			0x01
#define	IPR_B_TX				0x02
#define	IPR_B_RX				0x04
#define	IPR_A_EXTSTAT			0x08
#define	IPR_A_TX				0x10
#define	IPR_A_RX				0x20


/* RR7: FS_FIFO_HIGH_REG "FFHR" */

#define	FFHR_CNT_MASK			0x3f
#define	FFHR_IS_FROM_FIFO		0x40
#define	FFHR_FIFO_OVERRUN		0x80


/* RR10: DPLL_STATUS_REG "DSR" */

#define	DSR_ON_LOOP				0x02
#define	DSR_ON_LOOP_SENDING		0x10
#define	DSR_TWO_CLK_MISSING		0x40
#define	DSR_ONE_CLK_MISSING		0x80



/***************************** Prototypes *****************************/

int atari_SCC_init( void );

/************************* End of Prototypes **************************/

/* Compute the channel number from the base address */

#define CHANNR(info)	(((info)->port & 4) != ChannelsReversed)


/***********************************************************************/
/*                                                                     */
/*                               Constants                             */
/*                                                                     */
/***********************************************************************/


/***********************************************************************/
/*                                                                     */
/*                             Register Access                         */
/*                                                                     */
/***********************************************************************/


/* The SCC needs 3.5 PCLK cycles recovery time between to register
 * accesses. PCLK runs with 8 MHz on an Atari, so this delay is 3.5 *
 * 125 ns = 437.5 ns. Since this is too short for udelay(), it is
 * implemented by some nop's. I think that a nop needs 4 cycles (but
 * I'm not sure, correct me please!), that gives 4 nops for a TT (32
 * MHz) (2 would be sufficient for the Falcon (16 MHz), but looking at
 * boot_info.bi_atari.model at runtime takes longer than 2 nop's...)
 * ++andreas: nop needs only 2 cycles, seven of them are needed.
 */

/* 10/16/95: A tstb mfp.par_dt_reg takes 600ns (sure?) and thus should be
 * quite right
 */

static __inline__ void scc_reg_delay( void )
{
	MFPDELAY();
}

/* Another version with only 3 nop's for cases when some other
 * statement intervenes between the two SCC accesses
 * ++andreas: 3 nop's added
 * 10/16/95: use MFPDELAY, too.
 */

static __inline__ void scc_reg3_delay( void )
{
	MFPDELAY();
}


struct PARTIAL_SCC {		/* just one channel */
	unsigned char	ctrl;
	unsigned char	dummy;
	unsigned char	data;
};


extern unsigned char SCC_shadow[2][16];
extern int ChannelsReversed;


/* The following functions should relax the somehow complicated
 * register access of the SCC. _SCCwrite() stores all written values
 * (except for WR0 and WR8) in shadow registers for later recall. This
 * removes the burden of remembering written values as needed. The
 * extra work of storing the value doesn't count, since a delay is
 * needed after a SCC access anyway. Additionally, _SCCwrite() manages
 * writes to WR0 and WR8 differently, because these can be accessed
 * directly with less overhead. Another special case are WR7 and WR7'.
 * _SCCwrite automatically checks what of this registers is selected
 * and changes b0 of WR15 if needed.
 * 
 * _SCCread() for standard read registers is straightforward, except
 * for RR2 (split into two "virtual" registers: one for the value
 * written to WR2 (from the shadow) and one for the vector including
 * status from RR2, Ch. B) and RR3. The latter must be read from
 * Channel A, because it reads as all zeros on Ch. B. RR0 and RR8 can
 * be accessed directly as before.
 * 
 * The two inline function contain complicated switch statements. But
 * I rely on regno and final_delay being constants, so gcc can reduce
 * the whole stuff to just some assembler statements.
 * 
 * _SCCwrite and _SCCread aren't intended to be used directly under
 * normal circumstances. The macros SCCread[_ND] and SCCwrite[_ND] are
 * for that purpose. They assume that a local variable 'info' is
 * declared and pointing to the port's async_struct entry. The
 * variants with "_NB" appended should be used if no other SCC
 * accesses follow immediatly (within 0.5 usecs). They just skip the
 * final delay nops.
 * 
 * Please note that accesses to SCC registers should only take place
 * when interrupts are turned off (at least if SCC interrupts are
 * enabled). Otherwise, an interrupt could interfere with the
 * two-stage accessing process.
 *
 */

static __inline__ void _SCCwrite( struct async_struct *info, int regno,
				 unsigned char val, int final_delay )

{
	volatile struct PARTIAL_SCC *sc =
		(volatile struct PARTIAL_SCC *)info->port;
	unsigned char *shadow = SCC_shadow[CHANNR(info)];

	switch( regno ) {

	  case COMMAND_REG:
		/* WR0 can be written directly without pointing */
		sc->ctrl = val;
		break;

	  case SYNC_CHAR_REG:
		/* For WR7, first set b0 of WR15 to 0, if needed */
		if (shadow[INT_CTRL_REG] & ICR_OPTIONREG_SELECT) {
			sc->ctrl = 15;
			shadow[INT_CTRL_REG] &= ~ICR_OPTIONREG_SELECT;
			scc_reg3_delay();
			sc->ctrl = shadow[INT_CTRL_REG];
			scc_reg_delay();
		}
		goto normal_case;
		
	  case SDLC_OPTION_REG:
		/* For WR7', first set b0 of WR15 to 1, if needed */
		if (!(shadow[INT_CTRL_REG] & ICR_OPTIONREG_SELECT)) {
			sc->ctrl = 15;
			shadow[INT_CTRL_REG] |= ICR_OPTIONREG_SELECT;
			scc_reg3_delay();
			sc->ctrl = shadow[INT_CTRL_REG];
			scc_reg_delay();
		}
		sc->ctrl = 7;
		shadow[8] = val;	/* WR7' shadowed at WR8 */
		scc_reg3_delay();
		sc->ctrl = val;
		break;

	  case TX_DATA_REG:		/* WR8 */
		/* TX_DATA_REG can be accessed directly */
		sc->data = val;
		break;

	  case MASTER_INT_CTRL:
		sc->ctrl = regno;
		val &= 0x3f;	/* bits 6..7 are the reset commands */
		SCC_shadow[0][regno] = val;
		scc_reg3_delay();
		sc->ctrl = val;
		break;

	  case DPLL_CTRL_REG:
		sc->ctrl = regno;
		val &= 0x1f;			/* bits 5..7 are the DPLL commands */
		shadow[regno] = val;
		scc_reg3_delay();
		sc->ctrl = val;
		break;

	  case 1 ... 6:	
	  case 10 ... 13:
	  case 15:
	  normal_case:
		sc->ctrl = regno;
		shadow[regno] = val;
		scc_reg3_delay();
		sc->ctrl = val;
		break;
		
	  default:
		printk( "Bad SCC write access to WR%d\n", regno );
		break;
		
	}

	if (final_delay)
		scc_reg_delay();
}


static __inline__ unsigned char _SCCread( struct async_struct *info,
					 int regno, int final_delay )

{	volatile struct PARTIAL_SCC *sc =
		(volatile struct PARTIAL_SCC *)info->port;
	unsigned char rv;
	unsigned char *shadow = SCC_shadow[CHANNR(info)];

	switch( regno ) {

		/* --- real read registers --- */
	  case STATUS_REG:
		rv = sc->ctrl;
		break;

	  case SPCOND_STATUS_REG:
	  case FS_FIFO_LOW_REG:
	  case FS_FIFO_HIGH_REG:
	  case DPLL_STATUS_REG:
	  normal_case:
		sc->ctrl = regno & 0x0f;
		scc_reg_delay();
		rv = sc->ctrl;
		break;
		
	  case INT_PENDING_REG:
		/* RR3: read only from Channel A! */
		sc = (volatile struct PARTIAL_SCC *)
			((info->port & ~4) ^ (ChannelsReversed << 2));
		goto normal_case;

	  case RX_DATA_REG:
		/* RR8 can be accessed directly */
		rv = sc->data;
		break;

	  case CURR_VECTOR_REG:
		/* RR2 (vector including status) from Ch. B */
		sc = (volatile struct PARTIAL_SCC *)
			((info->port | 4) ^ (ChannelsReversed << 2));
		goto normal_case;
		
		/* --- reading write registers: access the shadow --- */
	  case 1 ... 7:
	  case 10 ... 15:
		return shadow[regno]; /* no final delay! */

		/* WR7' is special, because it is shadowed at the place of WR8 */
	  case SDLC_OPTION_REG:
		return shadow[8]; /* no final delay! */

		/* WR9 is special too, because it is common for both channels */
	  case MASTER_INT_CTRL:
		return SCC_shadow[0][9]; /* no final delay! */

	  default:
		printk( "Bad SCC read access to %cR%d\n", (regno & 16) ? 'R' : 'W',
				regno & ~16 );
		break;
		
	}

	if (final_delay)
		scc_reg_delay();
	return rv;
}


#define	SCCwrite(reg,val)		_SCCwrite( info, (reg), (val), 1 )
#define	SCCwrite_NB(reg,val)	_SCCwrite( info, (reg), (val), 0 )
#define	SCCread(reg)			_SCCread( info, (reg), 1 )
#define	SCCread_NB(reg)			_SCCread( info, (reg), 0 )

#define SCCmod(reg,and,or)		SCCwrite((reg),(SCCread(reg)&(and))|(or))

#endif /* _ATARI_SCC_H */
