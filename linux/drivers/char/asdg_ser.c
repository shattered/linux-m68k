/* asdg_ser.c - Serial driver for ASDG DSB serial card */
/* This card is based on the Zilog 8530.  */

/* Copyright 1998 Shawn D'Alimonte - This is covered by the GPL */

/* Contact me at aa600@torfree.net */

/* This is really in a beta stage.  Please try it out and report bugs
 * or make some suggestions
 * 
 * I have tested it with minicom to a modem as well as getty and shell 
 * redirection to a nullmodemed PC.  
 *
 * Got 115k2 working!  Why can't Linux use a better way to represent baud rates?
 *
 * There are two kludges in this driver.  
 * 1) I use info->MCR to store the current RTS and DTR status since R5 is write
 *    only.  All other info in R5 can be obtained from termios structure.  If 
 *    this use of info->MCR is a problem, could someone please let me know.
 * 2) In the _change_speed() I borrowed a divisor of 0 is used as an 'invalid 
 * rate' flag.  Unfortunatly a divisor of 0 gives 115200bps on this card.  I put
 *  a value of -1 in the table and checked for this right before writing the 
 * divisor registers.  Later on I should change it so that -1 becomes the special 
 * flag and 0 is a valid divisor.
 * 
 * Build instructions:
 * If you got a .tgz file with a Makefile :
 * run make and use insmod (as root) at load the module. 
 * The ASDG ports get the first two available ttyS? devices
 * Amiga port 0 on the card gets the higher number ttyS? device (I know, I should 
 * fix that)
 * 
 * If you got a set of patches add it your kernel, and build a new kernel (Works
 *  as a module or part of the kernel.
 *
 * This code is covered by the GPL.  it is based heavily on
 * many of the existing serial drivers (amiga_ser, ser_ioext, ser_mfc, etc.)
 */

#include <linux/module.h>

#include <linux/types.h>
#include <linux/sched.h>
#include <linux/interrupt.h>
#include <linux/termios.h>
#include <linux/tty.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/serial.h>
#include <linux/delay.h>

#include <asm/setup.h>
#include <asm/amigaints.h>
#include <asm/amigahw.h>
#include <asm/amigatypes.h>
#include <asm/zorro.h>
#include <asm/irq.h>

#include "asdg_ser.h"

/* how many cards are to be supported */
#define MAX_CARD 5

/* Allow it to build on non-modified kernels */
#ifndef SER_ASDG
#define SER_ASDG 109
#endif

int nr_asdg=0;
static int board_index[MAX_CARD];
static int lines[MAX_CARD * 2];

struct SCCHalf {
  u_char fill1;
  volatile u_char control;
  u_char fill2;
  volatile u_char data;
};

struct SCC {
  struct SCCHalf B;
  struct SCCHalf A;
};

/* Standard speeds table */
static int baud_table[18] = {
    /* B0     */ 0,
    /* B50    */ 4606,
    /* B75    */ 3070,
    /* B110   */ 2093,
    /* B134   */ 1717,
    /* B150   */ 1534,
    /* B200   */ 1150,
    /* B300   */ 766,
    /* B600   */ 382,
    /* B1200  */ 190,
    /* B1800  */ 126,
    /* B2400  */ 94,
    /* B4800  */ 46,
    /* B9600  */ 22,
    /* B19200 */ 10,
    /* B38400 */ 4,
    /* B57600 */ 2,
    /* 115200 */ -1
};

/* Prototypes */
static inline void write_zsdata(struct SCCHalf *channel, unsigned char value);
static inline unsigned char read_zsdata(struct SCCHalf *channel);
static inline void write_zsreg(struct SCCHalf *channel, unsigned char reg, 
			       unsigned char value);
static inline unsigned char read_zsreg(struct SCCHalf *channel,
				       unsigned char reg);
static void asdg_deinit(struct async_struct *info, int leave_dtr);
static void asdg_init(struct async_struct *info);
static void asdg_interrupt(int irq, void *data, struct pt_regs *fp);
static int asdg_trans_empty(struct async_struct *info);
static void asdg_stop_receive(struct async_struct *info);
static int asdg_set_modem_info(struct async_struct *info, int new_dtr, int new_rts);
static unsigned int asdg_get_modem_info(struct async_struct *info);
static void asdg_get_serial_info(struct async_struct *info, struct serial_struct *retinfo);
static void asdg_set_break(struct async_struct *info, int break_flag);
static void asdg_throttle(struct async_struct *info, int status);
static void asdg_change_speed(struct async_struct *info);
static int asdg_check_custom_divisor(struct async_struct *info, int baud_base, int divisor);
static void asdg_enab_tx_int(struct async_struct *info, int enab_flag);
/* End prototypes */

static SERIALSWITCH asdg_ser_switch = {
    asdg_init, 
    asdg_deinit, 
    asdg_enab_tx_int,
    asdg_check_custom_divisor, 
    asdg_change_speed,
    asdg_throttle, 
    asdg_set_break,
    asdg_get_serial_info, 
    asdg_get_modem_info,
    asdg_set_modem_info, 
    NULL,
    asdg_stop_receive, 
    asdg_trans_empty, 
    NULL
};


static void asdg_enab_tx_int(struct async_struct *info, int enab_flag)
{
  
  int ch; 
  struct SCCHalf *port = (struct SCCHalf *)info->port;

#ifdef DEBUG
  printk("asdg_enab_tx_int: enab_flag= %d\n", enab_flag);
#endif 

  if(enab_flag) {
    if( (ch=rs_get_tx_char(info)) >=0)
      {
#ifdef DEBUG
	printk("{%x}", ch);
#endif
	write_zsdata(port, ch);
      }
    else {
#ifdef DEBUG
      	printk("{%x}", ch);
#endif
      write_zsreg(port, R0, RES_Tx_P);
    }
  } 
  else 
    write_zsreg(port, R0, RES_Tx_P);
}

static int asdg_check_custom_divisor(struct async_struct *info, int baud_base, int divisor)
{
#ifdef DEBUG
  printk("asdg_check_custom_divisor: base %d, div %d\n", baud_base, divisor);
#endif
  if( (divisor>=0) && (divisor<=0xffff))
    return 0;
  
  return 1;
}

/* The ASDG card uses a value of 0 for the clock divider at 115200bps.  
 * The code seems to use 0 as a flag, so I used -1 and checked for this 
 * special value just before it is writen to the divider register.  There 
 * is probably a better way to handle this, but it works.  Maybe -1 used be 
 * used as the flag?*/
static void asdg_change_speed(struct async_struct *info)
{
    unsigned	cflag, baud, chsize, stopb, parity, aflags;
    unsigned	div = 0;
    int realbaud;
    u_char val;
	
#ifdef DEBUG
    printk("asdg_change_speed\n");
#endif

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
    if (!div){
	if (baud > 17) baud = 17;
	realbaud = baud_table[baud];
	if (realbaud)
	  div=realbaud;
    }

    if (div) {
      asdg_set_modem_info(info, 1, -1); /* DTR on */
    } else {
	    /* speed == 0 -> drop DTR */
      asdg_set_modem_info(info, 0, -1);
	    return;
    }

    /* setup the serial port period register */
    /* Kludge for div=0 (115k2 bps) */
    if(div==-1) div=0;
    write_zsreg((struct SCCHalf *)info->port, R12, div%256); /* Low byte */
    write_zsreg((struct SCCHalf *)info->port, R13, div/256); /* High byte */

    /* Set character length (data bits) */
    val = TxENAB;
    val |= (info->MCR & MCR_DTR) ? DTR : 0 ;
    val |= (info->MCR & MCR_RTS) ? RTS : 0 ;
    val |= (info->MCR & MCR_BREAK) ? SND_BRK : 0;

    if(chsize==CS5) {
      write_zsreg((struct SCCHalf *)info->port, R3, Rx5 | RxENABLE);
      val |= Tx5;
    } else if(chsize==CS6) {
      write_zsreg((struct SCCHalf *)info->port, R3, Rx6 | RxENABLE );
      val |= Tx6;
    } else if(chsize==CS7) {
      write_zsreg((struct SCCHalf *)info->port, R3, Rx7 | RxENABLE );
      val |= Tx7;
    } else if(chsize==CS8) {
      write_zsreg((struct SCCHalf *)info->port, R3, Rx8 | RxENABLE );
      val |= Tx8;
    } else {/* Should never get here! */
      printk("asdg_ser: change_speed - illegal char size\n");
    }

    write_zsreg((struct SCCHalf *)info->port, R5, val);

    /* Parity and stop bits */
    val = X16CLK;
    val |= stopb ? SB2 : SB1;
    val |= (parity & PARENB) ? PAR_ENA : 0;
    val |= (parity & PARODD) ? 0: PAR_EVEN;
    write_zsreg((struct SCCHalf *)info->port, R4, val);
}

static void asdg_throttle(struct async_struct *info, int status)
{
#ifdef DEBUG 
   printk("asdg_throttle: %d\n", status);
#endif

   if(status)
    asdg_set_modem_info(info, -1, 0);  /* Lower RTS */
   else
    asdg_set_modem_info(info, -1, 1); /* Raise RTS */
}

static void asdg_set_break(struct async_struct *info, int break_flag)
{
  unsigned cflag, chsize;
  u_char val = TxENAB;

#ifdef DEBUG
    printk("asdg_set_break: %d\n", break_flag);
#endif
    if (!info->tty || !info->tty->termios) return;

    cflag  = info->tty->termios->c_cflag;
    chsize = cflag & CSIZE;

    val |= (info->MCR & MCR_DTR) ? DTR : 0; 
    val |= (info->MCR & MCR_RTS) ? RTS : 0;
    if(chsize==CS5)
      val |= Tx5;
    else if(chsize==CS6)
      val |= Tx6;
    else if(chsize==CS7)
      val |= Tx7;
    else
      val |= Tx8;

    if(break_flag) {
      val |= SND_BRK;
      info->MCR |= MCR_BREAK;
    } else
      info->MCR &= ~MCR_BREAK;

#ifdef DEBUG
    printk("asdg_set_break: %x -> R5\n", val);
#endif
    write_zsreg((struct SCCHalf *)info->port, R5, val);
}

static void asdg_get_serial_info(struct async_struct *info, struct serial_struct *retinfo)
{
#ifdef DEBUG
   printk("asdg_get_serial_info\n");
#endif

  retinfo->baud_base=7372800;
  retinfo->custom_divisor=info->custom_divisor;
}

static unsigned int asdg_get_modem_info(struct async_struct *info)
{
  u_char status=read_zsreg((struct SCCHalf *)info->port, R0);
  
#ifdef DEBUG
  printk("asdg_get_modem_info\n");
#endif

  return(
	 ( (info->MCR & MCR_DTR) ? TIOCM_DTR : 0) |
	 ( (info->MCR & MCR_RTS) ? TIOCM_RTS : 0) |
	 ( (status & DCD) ? TIOCM_CAR : 0) |
	 ( (status & CTS) ? 0 : TIOCM_CTS) |
	 /* TICM_RNG */ 0);
}

static int asdg_set_modem_info(struct async_struct *info, int new_dtr, int new_rts)
{
  unsigned cflag, chsize;
  u_char val = TxENAB;

#ifdef DEBUG
  printk("asdg_set_modem_info  dtr: %d, rts %d\n", new_dtr, new_rts);
#endif

  if (!info->tty || !info->tty->termios) return 0;
  
  cflag  = info->tty->termios->c_cflag;
  chsize = cflag & CSIZE;

  if(chsize==CS5)
    val|=Tx5;
  else if(chsize==CS6)
    val|=Tx6;
  else if(chsize==CS7)
    val|=Tx7;
  else
    val|=Tx8;

  if(new_dtr == 1) {
    info->MCR |= MCR_DTR;
    val |= DTR;
  } else if(new_dtr == 0) {
    info->MCR &= ~MCR_DTR;
    val &= ~DTR;
  } else if(new_dtr == -1) {
    val |= (info->MCR & MCR_DTR)?DTR:0;
  }

  if(new_rts == 1) {
    info->MCR |= MCR_RTS;
    val |= RTS;
  } else if(new_rts == 0) {
    info->MCR &= ~MCR_RTS;
  } else if(new_rts == -1) {
    val |= (info->MCR & MCR_RTS)?RTS:0;
  }

  if(info->MCR & MCR_BREAK) val |= SND_BRK;

#ifdef DEBUG
  printk("asdg_set_modem_info: %x -> R5\n", val);
#endif

  write_zsreg((struct SCCHalf *)info->port, R5, val);

  return 0;
}

static void asdg_stop_receive(struct async_struct *info)
{
#ifdef DEBUG
    printk("asdg_stop_receive\n");
#endif

  write_zsreg((struct SCCHalf *)info->port, R3, Rx8); /* Turn off Rx */
  write_zsreg((struct SCCHalf *)info->port, R1, 0);   /* Turn off Tx */
}

/* Return !=0 iff no more characters in Tx FIFO */
static int asdg_trans_empty(struct async_struct *info)
{
#ifdef DEBUG
  printk("asdg_trans_empty");
#endif

  return read_zsreg((struct SCCHalf *)info->port, R1) & ALL_SNT;
}

/* Interrupt Service Routine */
static void asdg_interrupt(int irq, void *data, struct pt_regs *fp)
{
  int i;
  u_char ivec; /* Interrupt vector - not sure if it is needed */
  u_char ip;   /* IP bits from card */
  int ch; /* Received or xmitted char */
  struct SCC *scc;
  struct async_struct *infoA;
  struct async_struct *infoB;

  /* Check all cards */
  for(i=0; i<nr_asdg; i+=2) {

    infoB = &rs_table[lines[i]];
    infoA = &rs_table[lines[i+1]];
    scc = (struct SCC *)infoA->board_base;
    
    ivec=read_zsreg(&scc->B, R2); /* Get Interrupt Vector and ignore it :-) */
    ip=read_zsreg(&scc->A, R3); /* Get IP bits */

    if(ip & CHBEXT) { /* Channel B ext/status */
#ifdef INTDEBUG
      printk("Channel B Ext/Stat");
#endif
      write_zsreg(&scc->B, R0, RES_EXT_INT); 
    }

    if(ip & CHBTxIP) {        /* Channel B TBE */
#ifdef INTDEBUG
      printk("Chan B TBE\n");
#endif
      if( (ch=rs_get_tx_char(infoB)) >=0 ) {
#ifdef INTDEBUG
	//	printk("{%x}", ch);
#endif
	write_zsdata(&scc->B, ch);
      }
      else {
#ifdef INTDEBUG
	//	printk("{%x}", ch);
#endif
	write_zsreg(&scc->B, R0, RES_Tx_P);
      }
    }


    if(ip & CHBRxIP) { /* Channel B Rx Char Avail */
#ifdef INTDEBUG
            printk("Channel B Rx char avail");
#endif
      while( (read_zsreg(&scc->B, R0) & Rx_CH_AV) != 0)
	{
	  ch=read_zsdata(&scc->B);
	  rs_receive_char(infoB, ch , 0);
#ifdef INTDEBUG
	  printk("Received Char Chan B: %c\n", ch);
#endif
	}
    }

    if(ip & CHAEXT) { /* Channel A ext/status */
#ifdef INTDEBUG
      printk("Channel A Ext/Stat");
#endif
      write_zsreg(&scc->A, R0, RES_EXT_INT); 
    }

    if(ip & CHATxIP) {/* Channel A TBE */
#ifdef INTDEBUG
      printk("Chan A TBE\n");
#endif
      if( (ch=rs_get_tx_char(infoA)) >=0) {
#ifdef INTDEBUG
	//	printk("{%x}", ch);
#endif
	write_zsdata(&scc->A, ch);
      }
      else {
#ifdef INTDEBUG
	//	printk("{%x}", ch);
#endif
	write_zsreg(&scc->A, R0, RES_Tx_P);
      }
    }

    if(ip & CHARxIP) { /* Channel A Rx Char Avail */
#ifdef INTDEBUG
            printk("Channel A Rx char avail");
#endif
      while( (read_zsreg(&scc->A, R0) & Rx_CH_AV) != 0)
	{
	  ch=read_zsdata(&scc->A);
	  rs_receive_char(infoA, ch , 0);
#ifdef INTDEBUG
	  printk("Received Char Chan A: %c\n", ch);
#endif
	}
    }
  }
}
 

/* Initialize port, turn on ints and set DTR, RTS */
static void asdg_init(struct async_struct *info)
{
  struct SCCHalf *port = (struct SCCHalf *)info->port;

#ifdef DEBUG
  printk("asdg_init: ");
#endif

  /* Enable ints */
#ifdef DEBUG
  printk("Port: %p Interrupts on\n", port);
#endif
  write_zsreg(port, R1, TxINT_ENAB | INT_ALL_Rx);

  /* Enable Rx and Tx */
  write_zsreg(port, R3, RxENABLE | Rx8 );

#ifdef DEBUG
  printk("asdg_init: %x -> R5\n", TxENAB | Tx8);
#endif
  write_zsreg(port, R5, TxENAB | Tx8);

  info->MCR=0; /* Clear our status info */

  asdg_set_modem_info(info, 1, 1);
}

/* Shutdown the port - turn off ints */
static void asdg_deinit(struct async_struct *info, int leave_dtr)
{
  struct SCCHalf *port= (struct SCCHalf *)info->port;

#ifdef DEBUG
  printk("asdg_deinit: %p\n", port);
#endif

  /* Turn off interrupts */
#ifdef DEBUG
  printk("Port %p Interrupts off\n", port);
#endif
  write_zsreg(port, R1, 0);

  /* Drop RTS, DTR */
  asdg_set_modem_info(info, 0, 0);

  write_zsreg(port, R5, Tx8);
#ifdef DEBUG
  printk("asdg_deinit: %x -> R5\n", Tx8);
#endif
  write_zsreg(port, R3, Rx8);
}

int asdg_setup(void)
{
  int i, line1, line2;
  struct SCC *scc=NULL;
  int dummy;
  struct ConfigDev *cd=NULL;
  struct serial_struct req;
  struct async_struct *info=NULL;
  int CardFound=0; 
  
  if (!MACH_IS_AMIGA)
    return -ENODEV;
  
#ifdef DEBUG
  printk("We are a miggy\n");
#endif

  nr_asdg = 0;
  
  while((i=zorro_find(MANUF_ASDG, PROD_TWIN_X,1, 0))) {

    CardFound=1;

    board_index[nr_asdg/2] = i;
    cd = zorro_get_board(i);
    scc = (struct SCC *)ZTWO_VADDR((((volatile u_char *)cd->cd_BoardAddr)));

#ifdef DEBUG
    printk("Found ASDG DSB at %p\n", scc);
#endif
    
    req.line = -1; /* first free ttyS? device */
    req.type = SER_ASDG;
    req.port = (int) &(scc->B);
    if ((line1 = register_serial( &req )) < 0) {
      printk( "Cannot register ASDG serial port: no free device\n" );
      return -EBUSY;
    }
    lines[nr_asdg++] = line1;
    info = &rs_table[line1];
    info->sw = &asdg_ser_switch;
    info->nr_uarts = nr_asdg;
    info->board_base = scc;
    
    req.line = -1; /* first free ttyS? device */
    req.type = SER_ASDG;
    req.port = (int) &(scc->A);
    if ((line2 = register_serial( &req )) < 0) {
      printk( "Cannot register ASDG serial port: no free device\n" );
      unregister_serial( line1 );
      return -EBUSY;
    }
    lines[nr_asdg++] = line2;
    info = &rs_table[line2];
    info->sw = &asdg_ser_switch;
    info->nr_uarts = nr_asdg--;
    info->board_base = scc;
    
    nr_asdg++;
    
    zorro_config_board(i,1);

    /* Clear pointers */
    dummy = scc->A.control;
    dummy = scc->B.control;
    
#ifdef DEBUG
    printk("Pointers cleared \n");
#endif

    /* Reset card */
    write_zsreg(&scc->A, R9, FHWRES | MIE);
    udelay(10); /* Give card time to reset */
    write_zsreg(&scc->B, R9, FHWRES | MIE);
    udelay(10); /* Give card time to reset */

#ifdef DEBUG
    printk("Card reset - MIE on \n");
#endif

    /* Reset all potential interupt sources */
    write_zsreg(&scc->A, R0, RES_EXT_INT);  /* Ext ints (disabled anyways) */
    write_zsreg(&scc->B, R0, RES_EXT_INT); 

#ifdef DEBUG
    printk("Ext ints cleared\n");
#endif

    write_zsreg(&scc->A, R0, RES_Tx_P);     /* TBE int */
    write_zsreg(&scc->B, R0, RES_Tx_P);

#ifdef DEBUG
    printk("TBE Cleared\n");
#endif

    /* Clear Rx FIFO */
    while( (read_zsreg(&scc->A, R0) & Rx_CH_AV) != 0)
      dummy=read_zsdata(&scc->A);
    while( (read_zsreg(&scc->B, R0) & Rx_CH_AV) != 0) 
      dummy=read_zsdata(&scc->B);

#ifdef DEBUG
    printk("Rx buffer empty\n");
#endif
    
    /* TBE and RX int off - we will turn them on in _init()*/
    write_zsreg(&scc->A, R1, 0);
    write_zsreg(&scc->B, R1, 0);

#ifdef DEBUG    
    printk("Tx and Rx ints off \n");
#endif

    /* Interrupt vector */
    write_zsreg(&scc->A, R2, 0);
    write_zsreg(&scc->B, R2, 0);

#ifdef DEBUG
    printk("Int vector set (unused) \n");
#endif

    write_zsreg(&scc->A, R3, Rx8);
    write_zsreg(&scc->B, R3, Rx8);

#ifdef DEBUG
    printk("Rx enabled\n");
#endif

    write_zsreg(&scc->A, R4, SB1 | X16CLK);
    write_zsreg(&scc->B, R4, SB1 | X16CLK);

#ifdef DEBUG
    printk("1 stop bit, x16 clock\n");
#endif

    write_zsreg(&scc->A, R5, Tx8);
    write_zsreg(&scc->B, R5, Tx8);
    
#ifdef DEBUG
    printk("Tx enabled \n");
#endif

    write_zsreg(&scc->A, R10, NRZ); 
    write_zsreg(&scc->B, R10, NRZ); 

#ifdef DEBUG
    printk("NRZ mode\n");
#endif

    write_zsreg(&scc->A, R11, TCBR | RCBR | TRxCBR);
    write_zsreg(&scc->B, R11, TCBR | RCBR | TRxCBR);

#ifdef DEBUG
    printk("Clock source setup\n");
#endif

    /*300 bps */
    write_zsreg(&scc->A, R12, 0xfe);
    write_zsreg(&scc->A, R13, 2);
    write_zsreg(&scc->B, R12, 0xfe);
    write_zsreg(&scc->B, R13, 2);

#ifdef DEBUG
    printk("Baud rate set - 300\n");
#endif

    write_zsreg(&scc->A, R14, BRENABL | BRSRC);
    write_zsreg(&scc->B, R14, BRENABL | BRSRC);

#ifdef DEBUG
    printk("BRG enabled \n");
#endif

    write_zsreg(&scc->A, R15, 0);
    write_zsreg(&scc->A, R15, 0);

#ifdef DEBUG
    printk("Ext INT IE bits cleared \n");
#endif
  }

  if(CardFound) {
    request_irq(IRQ_AMIGA_EXTER, asdg_interrupt, 0, "ASDG serial",
		asdg_interrupt);
    return 0;
  } else {
    printk("No ASDG Cards found\n");
    return -ENODEV;
  }
}

#ifdef MODULE
int init_module(void)
{
return asdg_setup();
}

void cleanup_module(void)
{
  int i;

  for (i = 0; i < nr_asdg; i += 2) {

    struct SCC *scc = (struct SCC *)rs_table[lines[i]].board_base;

    write_zsreg(&scc->A, R9, FHWRES);
    udelay(10); /* Give card time to reset */
    write_zsreg(&scc->B, R9, FHWRES);
    udelay(10); /* Give card time to reset */

    unregister_serial(lines[i]);
    unregister_serial(lines[i+1]);
    zorro_unconfig_board(board_index[i/2], 1);
  }
  free_irq(IRQ_AMIGA_EXTER, asdg_interrupt);
}
#endif
   
/* 
 * Reading and writing Z8530 registers.
 */

static inline unsigned char read_zsreg(struct SCCHalf *channel,
				       unsigned char reg)
{
  unsigned char retval;
  
  if (reg != 0) {
    channel->control = reg;
    RECOVERY_DELAY;
  }
  retval = channel->control;
  RECOVERY_DELAY;
#ifdef IO_DEBUG
  if(reg==5) printk("<rp%d,%2.2x>", reg, retval);
#endif
  return retval;
}

static inline void write_zsreg(struct SCCHalf *channel,
			       unsigned char reg, unsigned char value)
{
  if (reg != 0) {
    channel->control = reg;
    RECOVERY_DELAY;
  }
  channel->control = value;
  RECOVERY_DELAY;
#ifdef IO_DEBUG
  if(reg==5) printk("<wp%d,%2.2x>", reg, value);
#endif
  return;
}

static inline unsigned char read_zsdata(struct SCCHalf *channel)
{
  unsigned char retval;
  
  retval = channel->data;
  RECOVERY_DELAY;
#ifdef IO_DEBUG
  //printk("[<%c]", retval);
#endif
  return retval;
}

static inline void write_zsdata(struct SCCHalf *channel,
				unsigned char value)
{
#ifdef IO_DEBUG
  //  printk("[>%c]", value);
#endif
  channel->data = value;
  RECOVERY_DELAY;
  return;
}



