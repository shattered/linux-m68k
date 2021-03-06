/* $Id: hisax.h,v 1.13.2.19 1998/11/05 21:11:17 keil Exp $

 *   Basic declarations, defines and prototypes
 *
 * $Log: hisax.h,v $
 * Revision 1.13.2.19  1998/11/05 21:11:17  keil
 * AVM PnP support
 *
 * Revision 1.13.2.18  1998/11/03 00:06:33  keil
 * certification related changes
 * fixed logging for smaller stack use
 *
 * Revision 1.13.2.17  1998/10/11 19:33:48  niemann
 * Added new IPAC based cards.
 * Code cleanup and simplified (sedlbauer.c)
 *
 * Revision 1.13.2.16  1998/10/04 23:04:54  keil
 * ISAR works now
 *
 * Revision 1.13.2.15  1998/09/30 22:28:04  keil
 * more work for isar support
 *
 * Revision 1.13.2.14  1998/09/27 13:06:09  keil
 * Apply most changes from 2.1.X (HiSax 3.1)
 *
 * Revision 1.13.2.13  1998/08/25 14:01:30  calle
 * Ported driver for AVM Fritz!Card PCI from the 2.1 tree.
 * I could not test it.
 *
 * Revision 1.13.2.12  1998/07/15 14:43:33  calle
 * Support for AVM passive PCMCIA cards:
 *    A1 PCMCIA, FRITZ!Card PCMCIA and FRITZ!Card PCMCIA 2.0
 *
 * Revision 1.13.2.11  1998/05/27 18:05:30  keil
 * HiSax 3.0
 *
 * Revision 1.13.2.10  1998/04/11 18:43:16  keil
 * New cards
 *
 * Revision 1.13.2.9  1998/03/07 23:15:21  tsbogend
 * made HiSax working on Linux/Alpha
 *
 * Revision 1.13.2.8  1998/02/11 14:23:10  keil
 * support for Dr Neuhaus Niccy PnP and PCI
 *
 * Revision 1.13.2.7  1998/02/09 11:21:22  keil
 * Sedlbauer PCMCIA support from Marcus Niemann
 *
 * Revision 1.13.2.6  1998/02/03 23:16:12  keil
 * german AOC
 *
 * Revision 1.13.2.5  1998/01/27 22:42:42  keil
 * changes for new teles 16.3c and dynalink ---> asuscom
 *
 * Revision 1.13.2.4  1998/01/11 22:55:17  keil
 * 16.3c support
 *
 * Revision 1.13.2.3  1997/11/27 12:31:59  keil
 * Working netjet driver
 *
 * Revision 1.13.2.2  1997/11/15 18:55:43  keil
 * New init, new cards
 *
 * Revision 1.13.2.1  1997/10/17 22:13:51  keil
 * update to last hisax version
 *
 * Revision 2.6  1997/09/11 17:25:51  keil
 * Add new cards
 *
 * Revision 2.5  1997/08/03 14:36:31  keil
 * Implement RESTART procedure
 *
 * Revision 2.4  1997/07/31 19:25:20  keil
 * PTP_DATA_LINK support
 *
 * Revision 2.3  1997/07/31 11:50:17  keil
 * ONE TEI and FIXED TEI handling
 *
 * Revision 2.2  1997/07/30 17:13:02  keil
 * more changes for 'One TEI per card'
 *
 * Revision 2.1  1997/07/27 21:45:13  keil
 * new main structures
 *
 * Revision 2.0  1997/06/26 11:06:27  keil
 * New card and L1 interface.
 * Eicon.Diehl Diva and Dynalink IS64PH support
 *
 * old changes removed KKe
 *
 */
#include <linux/config.h>
#include <linux/module.h>
#include <linux/version.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/major.h>
#include <asm/segment.h>
#include <asm/io.h>
#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/signal.h>
#include <linux/malloc.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/ioport.h>
#include <linux/timer.h>
#include <linux/wait.h>
#include <linux/isdnif.h>
#include <linux/tty.h>
#include <linux/serial_reg.h>

#define REQUEST		0
#define CONFIRM		1
#define INDICATION	2
#define RESPONSE	3

#define HW_ENABLE	0x0000
#define HW_RESET	0x0004
#define HW_POWERUP	0x0008
#define HW_ACTIVATE	0x0010
#define HW_DEACTIVATE	0x0018
#define HW_INFO2	0x0020
#define HW_INFO3	0x0030
#define HW_INFO4_P8	0x0040
#define HW_INFO4_P10	0x0048
#define HW_RSYNC	0x0060
#define HW_TESTLOOP	0x0070
#define CARD_RESET	0x00F0
#define CARD_SETIRQ	0x00F1
#define CARD_INIT	0x00F2
#define CARD_RELEASE	0x00F3
#define CARD_TEST	0x00F4
#define CARD_AUX_IND	0x00F5
#define CARD_LOAD_FIRM	0x00F6

#define PH_ACTIVATE	0x0100
#define PH_DEACTIVATE	0x0110
#define PH_DATA		0x0120
#define PH_PULL		0x0130
#define PH_TESTLOOP	0x0140
#define PH_PAUSE	0x0150
#define MPH_ACTIVATE	0x0180
#define MPH_DEACTIVATE	0x0190
#define MPH_INFORMATION	0x01A0

#define DL_ESTABLISH	0x0200
#define DL_RELEASE	0x0210
#define DL_DATA		0x0220
#define DL_FLUSH	0x0224
#define DL_UNIT_DATA	0x0230
#define MDL_ASSIGN	0x0280
#define MDL_REMOVE	0x0284
#define MDL_ERROR	0x0288
#define MDL_INFO_SETUP	0x02E0
#define MDL_INFO_CONN	0x02E4
#define MDL_INFO_REL	0x02E8

#define CC_SETUP	0x0300
#define CC_RESUME	0x0304
#define CC_MORE_INFO	0x0310
#define CC_IGNORE	0x0320
#define CC_REJECT	0x0324
#define CC_SETUP_COMPL	0x0330
#define CC_PROCEEDING	0x0340
#define CC_ALERTING	0x0344
#define CC_CONNECT	0x0350
#define CC_CHARGE	0x0354
#define CC_DISCONNECT	0x0360
#define CC_RELEASE	0x0368
#define CC_SUSPEND	0x0370
#define CC_T303		0x0383
#define CC_T304		0x0384
#define CC_T305		0x0385
#define CC_T308_1	0x0388
#define CC_T308_2	0x0389
#define CC_T310		0x0390
#define CC_T313		0x0393
#define CC_T318		0x0398
#define CC_T319		0x0399
#define CC_NOSETUP_RSP	0x03E0
#define CC_SETUP_ERR	0x03E1
#define CC_SUSPEND_ERR	0x03E2
#define CC_RESUME_ERR	0x03E3
#define CC_CONNECT_ERR	0x03E4
#define CC_RELEASE_ERR	0x03E5
#define CC_DLRL		0x03F0
#define CC_RESTART	0x03F4


#ifdef __KERNEL__

#define MAX_DFRAME_LEN	260
#define MAX_DFRAME_LEN_L1	300
#define HSCX_BUFMAX	4096
#define MAX_DATA_SIZE	(HSCX_BUFMAX - 4)
#define MAX_DATA_MEM	(HSCX_BUFMAX + 64)
#define RAW_BUFMAX	(((HSCX_BUFMAX*6)/5) + 5)
#define MAX_HEADER_LEN	4
#define MAX_WINDOW	8
#define MAX_MON_FRAME	32
#define MAX_DLOG_SPACE	2048
#define MAX_BLOG_SPACE	256

/* #define I4L_IRQ_FLAG SA_INTERRUPT */
#define I4L_IRQ_FLAG    0

/*
 * Statemachine
 */

struct FsmInst;

typedef void (* FSMFNPTR)(struct FsmInst *, int, void *);

struct Fsm {
	FSMFNPTR *jumpmatrix;
	int state_count, event_count;
	char **strEvent, **strState;
};

struct FsmInst {
	struct Fsm *fsm;
	int state;
	int debug;
	void *userdata;
	int userint;
	void (*printdebug) (struct FsmInst *, char *, ...);
};

struct FsmNode {
	int state, event;
	void (*routine) (struct FsmInst *, int, void *);
};

struct FsmTimer {
	struct FsmInst *fi;
	struct timer_list tl;
	int event;
	void *arg;
};

struct L3Timer {
	struct l3_process *pc;
	struct timer_list tl;
	int event;
};

#define FLG_L1_ACTIVATING	1
#define FLG_L1_ACTIVATED	2
#define FLG_L1_DEACTTIMER	3
#define FLG_L1_ACTTIMER		4
#define FLG_L1_T3RUN		5
#define FLG_L1_PULL_REQ		6

struct Layer1 {
	void *hardware;
	struct BCState *bcs;
	struct PStack **stlistp;
	int Flags;
	struct FsmInst l1m;
	struct FsmTimer	timer;
	void (*l1l2) (struct PStack *, int, void *);
	void (*l1hw) (struct PStack *, int, void *);
	void (*l1tei) (struct PStack *, int, void *);
	int mode, bc;
	int delay;
};

#define GROUP_TEI	127
#define TEI_SAPI	63
#define CTRL_SAPI	0
#define PACKET_NOACK	250

/* Layer2 Flags */

#define FLG_LAPB	0
#define FLG_LAPD	1
#define FLG_ORIG	2
#define FLG_MOD128	3
#define FLG_PEND_REL	4
#define FLG_L3_INIT	5
#define FLG_T200_RUN	6
#define FLG_ACK_PEND	7
#define FLG_REJEXC	8
#define FLG_OWN_BUSY	9
#define FLG_PEER_BUSY	10
#define FLG_DCHAN_BUSY	11
#define FLG_L1_ACTIV	12
#define FLG_ESTAB_PEND	13
#define FLG_PTP		14
#define FLG_FIXED_TEI	15

struct Layer2 {
	int tei;
	int sap;
	int maxlen;
	unsigned int flag;
	int vs, va, vr;
	int rc;
	int window;
	int sow;
	struct sk_buff *windowar[MAX_WINDOW];
	struct sk_buff_head i_queue;
	struct sk_buff_head ui_queue;
	void (*l2l1) (struct PStack *, int, void *);
	void (*l2l3) (struct PStack *, int, void *);
	void (*l2tei) (struct PStack *, int, void *);
	struct FsmInst l2m;
	struct FsmTimer t200, t203;
	int T200, N200, T203;
	int debug;
	char debug_id[16];
};

struct Layer3 {
	void (*l3l4) (struct PStack *, int, void *);
	void (*l3l2) (struct PStack *, int, void *);
	struct FsmInst l3m;
	struct sk_buff_head squeue;
	struct l3_process *proc;
	struct l3_process *global;
	int N303;
	int debug;
	char debug_id[8];
};

struct LLInterface {
	void (*l4l3) (struct PStack *, int, void *);
	void *userdata;
	void (*l1writewakeup) (struct PStack *, int);
	void (*l2writewakeup) (struct PStack *, int);
};


struct Management {
	int	ri;
	struct FsmInst tei_m;
	struct FsmTimer t202;
	int T202, N202, debug;
	void (*layer) (struct PStack *, int, void *);
};


struct Param {
	int cause;
	int loc;
	int bchannel;
	setup_parm setup;	/* from isdnif.h numbers and Serviceindicator */
	int chargeinfo;		/* Charge Info - only for 1tr6 in
				 * the moment
				 */
	int spv;		/* SPV Flag */
};


struct PStack {
	struct PStack *next;
	struct Layer1 l1;
	struct Layer2 l2;
	struct Layer3 l3;
	struct LLInterface lli;
	struct Management ma;
	int protocol;		/* EDSS1 or 1TR6 */
};

struct l3_process {
	int callref;
	int state;
	struct L3Timer timer;
	int N303;
	int debug;
	struct Param para;
	struct Channel *chan;
	struct PStack *st;
	struct l3_process *next;
};

struct hscx_hw {
	int hscx;
	int rcvidx;
	int count;              /* Current skb sent count */
	u_char *rcvbuf;         /* B-Channel receive Buffer */
};

struct isar_reg {
	unsigned int Flags;
	volatile u_char bstat;
	volatile u_char iis;
	volatile u_char cmsb;
	volatile u_char clsb;
	volatile u_char par[8];
};

struct isar_hw {
	int dpath;
	int rcvidx;
	int txcnt;
	int mml;
	u_char *rcvbuf;         /* B-Channel receive Buffer */
	struct isar_reg *reg;
};

struct hdlc_stat_reg {
	u_char cmd  __attribute__((packed));
	u_char xml  __attribute__((packed));
	u_char mode __attribute__((packed));
	u_char fill __attribute__((packed));
};

struct hdlc_hw {
	union {
		u_int ctrl;
		struct hdlc_stat_reg sr;
	} ctrl;
	u_int stat;
	int rcvidx;
	int count;              /* Current skb sent count */
	u_char *rcvbuf;         /* B-Channel receive Buffer */
};

struct hfcB_hw {
	unsigned int *send;
	int f1;
	int f2;
};

struct tiger_hw {
	u_int *send;
	u_int *s_irq;
	u_int *s_end;
	u_int *sendp;
	u_int *rec;
	int free;
	u_char *rcvbuf;
	u_char *sendbuf;
	u_char *sp;
	int sendcnt;
	u_int s_tot;
	u_int r_bitcnt;
	u_int r_tot;
	u_int r_err;
	u_int r_fcs;
	u_char r_state;
	u_char r_one;
	u_char r_val;
	u_char s_state;
};

struct amd7930_hw {
	u_char *tx_buff;
	u_char *rv_buff;
	int rv_buff_in;
	int rv_buff_out;
	struct sk_buff *rv_skb;
	struct hdlc_state *hdlc_state;
	struct tq_struct tq_rcv;
	struct tq_struct tq_xmt;
};


#define BC_FLG_INIT	1
#define BC_FLG_ACTIV	2
#define BC_FLG_BUSY	3
#define BC_FLG_NOFRAME	4
#define BC_FLG_HALF	5
#define BC_FLG_EMPTY	6

#define L1_MODE_NULL	0
#define L1_MODE_TRANS	1
#define L1_MODE_HDLC	2
#define L1_MODE_MODEM	7

struct BCState {
	int channel;
	int mode;
	int Flag;
	struct IsdnCardState *cs;
	int tx_cnt;		/* B-Channel transmit counter */
	struct sk_buff *tx_skb; /* B-Channel transmit Buffer */
	struct sk_buff_head rqueue;	/* B-Channel receive Queue */
	struct sk_buff_head squeue;	/* B-Channel send Queue */
	struct PStack *st;
	u_char *blog;
	struct timer_list transbusy;
	struct tq_struct tqueue;
	int event;
	int  (*BC_SetStack) (struct PStack *, struct BCState *);
	void (*BC_Close) (struct BCState *);
	union {
		struct hscx_hw hscx;
		struct hdlc_hw hdlc;
		struct isar_hw isar;
		struct hfcB_hw hfc;
		struct tiger_hw tiger;
		struct amd7930_hw  amd7930;
	} hw;
};

struct Channel {
	struct PStack *b_st, *d_st;
	struct IsdnCardState *cs;
	struct BCState *bcs;
	int chan;
	int incoming;
	struct FsmInst fi;
	struct FsmTimer drel_timer, dial_timer;
	int debug;
	int l2_protocol, l2_active_protocol;
	int l3_protocol;
	int data_open;
	struct l3_process *proc;
	setup_parm setup;	/* from isdnif.h numbers and Serviceindicator */
	int Flags;		/* for remembering action done in l4 */
	int leased;
};

struct elsa_hw {
	unsigned int base;
	unsigned int cfg;
	unsigned int ctrl;
	unsigned int ale;
	unsigned int isac;
	unsigned int itac;
	unsigned int hscx;
	unsigned int trig;
	unsigned int timer;
	unsigned int counter;
	unsigned int status;
	struct timer_list tl;
	unsigned int MFlag;
	struct BCState *bcs;
	u_char *transbuf;
	u_char *rcvbuf;
	unsigned int transp;
	unsigned int rcvp;
	unsigned int transcnt;
	unsigned int rcvcnt;
	u_char IER;
	u_char FCR;
	u_char LCR;
	u_char MCR;
	u_char ctrl_reg;
};

struct teles3_hw {
	unsigned int cfg_reg;
	signed   int isac;
	signed   int hscx[2];
	signed   int isacfifo;
	signed   int hscxfifo[2];
};

struct teles0_hw {
	unsigned int cfg_reg;
	unsigned int membase;
};

struct avm_hw {
	unsigned int cfg_reg;
	unsigned int isac;
	unsigned int hscx[2];
	unsigned int isacfifo;
	unsigned int hscxfifo[2];
	unsigned int counter;
};

struct ix1_hw {
	unsigned int cfg_reg;
	unsigned int isac_ale;
	unsigned int isac;
	unsigned int hscx_ale;
	unsigned int hscx;
};

struct diva_hw {
	unsigned int cfg_reg;
	unsigned int ctrl;
	unsigned int isac_adr;
	unsigned int isac;
	unsigned int hscx_adr;
	unsigned int hscx;
	unsigned int status;
	struct timer_list tl;
	u_char ctrl_reg;
};

struct asus_hw {
	unsigned int cfg_reg;
	unsigned int adr;
	unsigned int isac;
	unsigned int hscx;
	unsigned int u7;
	unsigned int pots;
};


struct hfc_hw {
	unsigned int addr;
	unsigned int fifosize;
	unsigned char cirm;
	unsigned char ctmt;
	unsigned char cip;
	u_char isac_spcr;
	struct timer_list timer;
};

struct sedl_hw {
	unsigned int cfg_reg;
	unsigned int adr;
	unsigned int isac;
	unsigned int hscx;
	unsigned int reset_on;
	unsigned int reset_off;
	struct isar_reg isar;
	unsigned int chip;
	unsigned int bus;
};

struct spt_hw {
	unsigned int cfg_reg;
	unsigned int isac;
	unsigned int hscx[2];
	unsigned char res_irq;
};

struct mic_hw {
	unsigned int cfg_reg;
	unsigned int adr;
	unsigned int isac;
	unsigned int hscx;
};

struct njet_hw {
	unsigned int base;
	unsigned int isac;
	unsigned int auxa;
	unsigned char auxd;
	unsigned char dmactrl;
	unsigned char ctrl_reg;
	unsigned char irqmask0;
	unsigned char irqstat0;
	unsigned char last_is0;
};

struct hfcD_hw {
	unsigned int addr;
	unsigned int bfifosize;
	unsigned int dfifosize;
	unsigned char cirm;
	unsigned char ctmt;
	unsigned char cip;
	unsigned char conn;
	unsigned char mst_m;
	unsigned char int_m1;
	unsigned char int_m2;
	unsigned char int_s1;
	unsigned char sctrl;
	unsigned char stat;
	unsigned char fifo;
	unsigned char f1;
	unsigned char f2;
	unsigned int *send;
	struct timer_list timer;
};

#define HW_IOM1		0
#define HW_IPAC		1
#define HW_ISAR		2
#define FLG_TWO_DCHAN	4
#define FLG_L1_DBUSY	5
#define FLG_DBUSY_TIMER 6
#define FLG_LOCK_ATOMIC 7
#define HW_MON0_RX_END	8
#define HW_MON1_RX_END	9
#define HW_MON0_TX_END	10
#define HW_MON1_TX_END	11

struct IsdnCardState {
	unsigned char typ;
	unsigned char subtyp;
	int protocol;
	unsigned int irq;
	int HW_Flags;
	int *busy_flag;
	union {
		struct elsa_hw elsa;
		struct teles0_hw teles0;
		struct teles3_hw teles3;
		struct avm_hw avm;
		struct ix1_hw ix1;
		struct diva_hw diva;
		struct asus_hw asus;
		struct hfc_hw hfc;
		struct sedl_hw sedl;
		struct spt_hw spt;
		struct mic_hw mic;
		struct njet_hw njet;
		struct hfcD_hw hfcD;
		struct ix1_hw niccy;
	} hw;
	int myid;
	isdn_if iif;
	u_char *status_buf;
	u_char *status_read;
	u_char *status_write;
	u_char *status_end;
	u_char (*readisac) (struct IsdnCardState *, u_char);
	void   (*writeisac) (struct IsdnCardState *, u_char, u_char);
	void   (*readisacfifo) (struct IsdnCardState *, u_char *, int);
	void   (*writeisacfifo) (struct IsdnCardState *, u_char *, int);
	u_char (*BC_Read_Reg) (struct IsdnCardState *, int, u_char);
	void   (*BC_Write_Reg) (struct IsdnCardState *, int, u_char, u_char);
	void   (*BC_Send_Data) (struct BCState *);
	int    (*cardmsg) (struct IsdnCardState *, int, void *);
	struct Channel channel[2];
	struct BCState bcs[2];
	struct PStack *stlist;
	u_char *rcvbuf;
	int rcvidx;
	struct sk_buff *tx_skb;
	int tx_cnt;
	int event;
	struct tq_struct tqueue;
	struct timer_list dbusytimer;
	struct sk_buff_head rq, sq; /* D-channel queues */
	int ph_state;
	int cardnr;
	char *dlog;
	int debug;
	u_char *mon_tx;
	u_char *mon_rx;
	int mon_txp;
	int mon_txc;
	int mon_rxp;
	u_char mocr;
	void   (*setstack_d) (struct PStack *, struct IsdnCardState *);
};

#define  MON0_RX	1
#define  MON1_RX	2
#define  MON0_TX	4
#define  MON1_TX	8

#define	 HISAX_MAX_CARDS	8

#define  ISDN_CTYPE_16_0	1
#define  ISDN_CTYPE_8_0		2
#define  ISDN_CTYPE_16_3	3
#define  ISDN_CTYPE_PNP		4
#define  ISDN_CTYPE_A1		5
#define  ISDN_CTYPE_ELSA	6
#define  ISDN_CTYPE_ELSA_PNP	7
#define  ISDN_CTYPE_TELESPCMCIA	8
#define  ISDN_CTYPE_IX1MICROR2	9
#define  ISDN_CTYPE_ELSA_PCMCIA	10
#define  ISDN_CTYPE_DIEHLDIVA	11
#define  ISDN_CTYPE_ASUSCOM	12
#define  ISDN_CTYPE_TELEINT	13
#define  ISDN_CTYPE_TELES3C	14
#define  ISDN_CTYPE_SEDLBAUER	15
#define  ISDN_CTYPE_SPORTSTER	16
#define  ISDN_CTYPE_MIC		17
#define  ISDN_CTYPE_ELSA_PCI	18
#define  ISDN_CTYPE_COMPAQ_ISA	19
#define  ISDN_CTYPE_NETJET	20
#define  ISDN_CTYPE_TELESPCI	21
#define  ISDN_CTYPE_SEDLBAUER_PCMCIA	22
#define  ISDN_CTYPE_AMD7930	23
#define  ISDN_CTYPE_NICCY	24
#define  ISDN_CTYPE_S0BOX	25
#define  ISDN_CTYPE_A1_PCMCIA	26
#define  ISDN_CTYPE_FRITZPCI	27
#define  ISDN_CTYPE_SEDLBAUER_FAX     28

#define  ISDN_CTYPE_COUNT	28

#ifdef ISDN_CHIP_ISAC
#undef ISDN_CHIP_ISAC
#endif

#ifndef __initfunc
#define __initfunc(__arginit) __arginit
#endif

#ifndef __initdata
#define __initdata
#endif

#define HISAX_INITFUNC(__arginit) __initfunc(__arginit)
#define HISAX_INITDATA __initdata

#ifdef	CONFIG_HISAX_16_0
#define  CARD_TELES0 (1<< ISDN_CTYPE_16_0) | (1<< ISDN_CTYPE_8_0)
#ifndef ISDN_CHIP_ISAC
#define ISDN_CHIP_ISAC 1
#endif
#else
#define  CARD_TELES0  0
#endif

#ifdef	CONFIG_HISAX_16_3
#define  CARD_TELES3 (1<< ISDN_CTYPE_16_3) | (1<< ISDN_CTYPE_PNP) | \
		     (1<< ISDN_CTYPE_TELESPCMCIA) | (1<< ISDN_CTYPE_COMPAQ_ISA)
#ifndef ISDN_CHIP_ISAC
#define ISDN_CHIP_ISAC 1
#endif
#else
#define  CARD_TELES3  0
#endif

#ifdef	CONFIG_HISAX_TELESPCI
#define  CARD_TELESPCI (1<< ISDN_CTYPE_TELESPCI)
#ifndef ISDN_CHIP_ISAC
#define ISDN_CHIP_ISAC 1
#endif
#else
#define  CARD_TELESPCI  0
#endif

#ifdef	CONFIG_HISAX_AVM_A1
#define  CARD_AVM_A1 (1<< ISDN_CTYPE_A1)
#ifndef ISDN_CHIP_ISAC
#define ISDN_CHIP_ISAC 1
#endif
#else
#define  CARD_AVM_A1  0
#endif

#ifdef	CONFIG_HISAX_AVM_A1_PCMCIA
#define  CARD_AVM_A1_PCMCIA (1<< ISDN_CTYPE_A1_PCMCIA)
#ifndef ISDN_CHIP_ISAC
#define ISDN_CHIP_ISAC 1
#endif
#else
#define  CARD_AVM_A1_PCMCIA  0
#endif

#ifdef	CONFIG_HISAX_FRITZPCI
#define  CARD_FRITZPCI (1<< ISDN_CTYPE_FRITZPCI)
#ifndef ISDN_CHIP_ISAC
#define ISDN_CHIP_ISAC 1
#endif
#else
#define  CARD_FRITZPCI  0
#endif

#ifdef	CONFIG_HISAX_ELSA
#define  CARD_ELSA (1<< ISDN_CTYPE_ELSA) | (1<< ISDN_CTYPE_ELSA_PNP) | \
		   (1<< ISDN_CTYPE_ELSA_PCMCIA) | (1<< ISDN_CTYPE_ELSA_PCI)
#ifndef ISDN_CHIP_ISAC
#define ISDN_CHIP_ISAC 1
#endif
#undef HISAX_INITFUNC
#define HISAX_INITFUNC(__arginit) __arginit
#undef HISAX_INITDATA
#define HISAX_INITDATA
#else
#define  CARD_ELSA  0
#endif


#ifdef	CONFIG_HISAX_IX1MICROR2
#define	CARD_IX1MICROR2 (1 << ISDN_CTYPE_IX1MICROR2)
#ifndef ISDN_CHIP_ISAC
#define ISDN_CHIP_ISAC 1
#endif
#else
#define CARD_IX1MICROR2 0
#endif

#ifdef  CONFIG_HISAX_DIEHLDIVA
#define CARD_DIEHLDIVA (1 << ISDN_CTYPE_DIEHLDIVA)
#ifndef ISDN_CHIP_ISAC
#define ISDN_CHIP_ISAC 1
#endif
#else
#define CARD_DIEHLDIVA 0
#endif

#ifdef  CONFIG_HISAX_ASUSCOM
#define CARD_ASUSCOM (1 << ISDN_CTYPE_ASUSCOM)
#ifndef ISDN_CHIP_ISAC
#define ISDN_CHIP_ISAC 1
#endif
#else
#define CARD_ASUSCOM 0
#endif

#ifdef  CONFIG_HISAX_TELEINT
#define CARD_TELEINT (1 << ISDN_CTYPE_TELEINT)
#ifndef ISDN_CHIP_ISAC
#define ISDN_CHIP_ISAC 1
#endif
#else
#define CARD_TELEINT 0
#endif

#ifdef  CONFIG_HISAX_SEDLBAUER
#define CARD_SEDLBAUER (1 << ISDN_CTYPE_SEDLBAUER) | (1 << ISDN_CTYPE_SEDLBAUER_PCMCIA) | ( 1 << ISDN_CTYPE_SEDLBAUER_FAX)
#ifndef ISDN_CHIP_ISAC
#define ISDN_CHIP_ISAC 1
#endif
#else
#define CARD_SEDLBAUER 0
#endif

#ifdef  CONFIG_HISAX_SPORTSTER
#define CARD_SPORTSTER (1 << ISDN_CTYPE_SPORTSTER)
#ifndef ISDN_CHIP_ISAC
#define ISDN_CHIP_ISAC 1
#endif
#else
#define CARD_SPORTSTER 0
#endif

#ifdef  CONFIG_HISAX_MIC
#define CARD_MIC (1 << ISDN_CTYPE_MIC)
#ifndef ISDN_CHIP_ISAC
#define ISDN_CHIP_ISAC 1
#endif
#else
#define CARD_MIC 0
#endif

#ifdef  CONFIG_HISAX_NETJET
#define CARD_NETJET (1 << ISDN_CTYPE_NETJET)
#ifndef ISDN_CHIP_ISAC
#define ISDN_CHIP_ISAC 1
#endif
#else
#define CARD_NETJET 0
#endif

#ifdef	CONFIG_HISAX_TELES3C
#define  CARD_TELES3C (1<< ISDN_CTYPE_TELES3C)
#else
#define  CARD_TELES3C  0
#endif

#ifdef  CONFIG_HISAX_AMD7930
#define CARD_AMD7930 (1 << ISDN_CTYPE_AMD7930)
#else
#define CARD_AMD7930 0
#endif

#ifdef	CONFIG_HISAX_NICCY
#define	CARD_NICCY (1 << ISDN_CTYPE_NICCY)
#ifndef ISDN_CHIP_ISAC
#define ISDN_CHIP_ISAC 1
#endif
#else
#define CARD_NICCY 0
#endif

#ifdef	CONFIG_HISAX_S0BOX
#define	CARD_S0BOX (1 << ISDN_CTYPE_S0BOX)
#ifndef ISDN_CHIP_ISAC
#define ISDN_CHIP_ISAC 1
#endif
#else
#define CARD_S0BOX 0
#endif

#define  SUPORTED_CARDS  (CARD_TELES0 | CARD_TELES3 | CARD_AVM_A1 | CARD_ELSA \
			 | CARD_IX1MICROR2 | CARD_DIEHLDIVA | CARD_ASUSCOM \
			 | CARD_TELEINT | CARD_SEDLBAUER | CARD_SPORTSTER \
			 | CARD_MIC | CARD_NETJET | CARD_TELES3C | CARD_AMD7930 \
			 | CARD_AVM_A1_PCMCIA | CARD_FRITZPCI\
			 | CARD_NICCY | CARD_S0BOX | CARD_TELESPCI)

#define TEI_PER_CARD 0

#ifdef CONFIG_HISAX_1TR6
#undef TEI_PER_CARD
#define TEI_PER_CARD 1
#endif

#ifdef CONFIG_HISAX_EURO
#undef TEI_PER_CARD
#define TEI_PER_CARD 1
#define HISAX_EURO_SENDCOMPLETE 1
#define EXT_BEARER_CAPS 1
#define HISAX_SEND_STD_LLC_IE 1
#ifdef	CONFIG_HISAX_NO_SENDCOMPLETE
#undef HISAX_EURO_SENDCOMPLETE
#endif
#ifdef	CONFIG_HISAX_NO_LLC
#undef HISAX_SEND_STD_LLC_IE
#endif
#undef HISAX_DE_AOC
#ifdef CONFIG_DE_AOC
#define HISAX_DE_AOC 1
#endif
#endif

/* L1 Debug */
#define	L1_DEB_WARN		0x01
#define	L1_DEB_INTSTAT		0x02
#define	L1_DEB_ISAC		0x04
#define	L1_DEB_ISAC_FIFO	0x08
#define	L1_DEB_HSCX		0x10
#define	L1_DEB_HSCX_FIFO	0x20
#define	L1_DEB_LAPD	        0x40
#define	L1_DEB_IPAC	        0x80
#define	L1_DEB_RECEIVE_FRAME    0x100
#define L1_DEB_MONITOR		0x200
#define DEB_DLOG_HEX		0x400
#define DEB_DLOG_VERBOSE	0x800

#define L2FRAME_DEBUG

#ifdef L2FRAME_DEBUG
extern void Logl2Frame(struct IsdnCardState *cs, struct sk_buff *skb, char *buf, int dir);
#endif

struct IsdnCard {
	int typ;
	int protocol;		/* EDSS1 or 1TR6 */
	unsigned int para[4];
	struct IsdnCardState *cs;
};

void init_bcstate(struct IsdnCardState *cs, int bc);

void setstack_HiSax(struct PStack *st, struct IsdnCardState *cs);
unsigned int random_ri(void);
void HiSax_addlist(struct IsdnCardState *sp, struct PStack *st);
void HiSax_rmlist(struct IsdnCardState *sp, struct PStack *st);

void setstack_l1_B(struct PStack *st);

void setstack_tei(struct PStack *st);
void setstack_manager(struct PStack *st);

void setstack_isdnl2(struct PStack *st, char *debug_id);
void releasestack_isdnl2(struct PStack *st);
void setstack_transl2(struct PStack *st);
void releasestack_transl2(struct PStack *st);

void setstack_l3dc(struct PStack *st, struct Channel *chanp);
void setstack_l3bc(struct PStack *st, struct Channel *chanp);
void releasestack_isdnl3(struct PStack *st);

u_char *findie(u_char * p, int size, u_char ie, int wanted_set);
int getcallref(u_char * p);
int newcallref(void);

void FsmNew(struct Fsm *fsm, struct FsmNode *fnlist, int fncount);
void FsmFree(struct Fsm *fsm);
int FsmEvent(struct FsmInst *fi, int event, void *arg);
void FsmChangeState(struct FsmInst *fi, int newstate);
void FsmInitTimer(struct FsmInst *fi, struct FsmTimer *ft);
int FsmAddTimer(struct FsmTimer *ft, int millisec, int event,
	void *arg, int where);
void FsmRestartTimer(struct FsmTimer *ft, int millisec, int event,
	void *arg, int where);
void FsmDelTimer(struct FsmTimer *ft, int where);
int jiftime(char *s, long mark);

int HiSax_command(isdn_ctrl * ic);
int HiSax_writebuf_skb(int id, int chan, struct sk_buff *skb);
void HiSax_putstatus(struct IsdnCardState *cs, char *head, char *fmt, ...);
void VHiSax_putstatus(struct IsdnCardState *cs, char *head, char *fmt, va_list args);
void HiSax_reportcard(int cardnr);
int QuickHex(char *txt, u_char * p, int cnt);
void LogFrame(struct IsdnCardState *cs, u_char * p, int size);
void dlogframe(struct IsdnCardState *cs, struct sk_buff *skb, int dir);
void iecpy(u_char * dest, u_char * iestart, int ieoffset);
int discard_queue(struct sk_buff_head *q);
#ifdef ISDN_CHIP_ISAC
void setstack_isac(struct PStack *st, struct IsdnCardState *cs);
#endif	/* ISDN_CHIP_ISAC */
#endif	/* __KERNEL__ */

#define HZDELAY(jiffs) {int tout = jiffs; while (tout--) udelay(1000000/HZ);}

int ll_run(struct IsdnCardState *cs);
void ll_stop(struct IsdnCardState *cs);
void CallcNew(void);
void CallcFree(void);
int CallcNewChan(struct IsdnCardState *cs);
void CallcFreeChan(struct IsdnCardState *cs);
void Isdnl1New(void);
void Isdnl1Free(void);
void Isdnl2New(void);
void Isdnl2Free(void);
void Isdnl3New(void);
void Isdnl3Free(void);
void init_tei(struct IsdnCardState *cs, int protocol);
void release_tei(struct IsdnCardState *cs);
char *HiSax_getrev(const char *revision);
void TeiNew(void);
void TeiFree(void);
int certification_check(int output);
