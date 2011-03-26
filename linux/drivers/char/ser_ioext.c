/*
 * This code is (C) 1995 Jes Sorensen (jds@kom.auc.dk).
 *
 * This is a driver for the GVP IO-Extender (duart, midi, parallel
 * board) for the Amiga (so far only the serial part is implemented).
 *
 * The code is partly based on Roman's Atari drivers, Hamish'
 * driver the internal Amiga port and the PC drivers.
 *
 * The board info was kindly supplied by Erick Quackenbush, former GVP
 * employee - thanks. The missing info on about the "secret" register
 * was obtained from the GVP SCSI driver though.
 *
 * Thanks to Niels Pontoppidan (neil@dannug.dk), for letting me borrow
 * his IO-Extender, and thus making it possible for me to do this
 * driver. Also thanks to Ralph Babel and Roman Hodek for a few hints
 * and to Michael Goodwin for doing all the beta-testing - especially
 * after I returned the board I borrowed from Niels.
 *
 * If you can use this code (or parts of it) - feel free to copy it,
 * but please let me know what parts you can use and where you use it.
 *
 * As the code probably isn't bug-free yet, I would also like to know
 * if you discover any bugs.
 *
 * Facts: 
 *
 * The tty-assignment is done dynamically so only the needed number of
 * tty's is allocated, leaving room for other serial boards in the
 * system. This needed some changes in serial.c aswell as the other
 * drivers, but I do find this the best way to handle things - I hate
 * static allocation!
 *
 * Note: To use the serial ports at rates higher than 38400, you can
 * compile the driver with IOEXT_FAST_RATES = 1, and then select
 * one of these rates:
 *
 * 300 = 57600, 1200 = 115200
 *
 * The driver should be able to handle multiple IO-Extenders, but I
 * haven't been able to test this as I only got access to one board at
 * the moment. The number of extenders is limited to five (same as the
 * maximum number of Zorro slots) but if you wan't to use more than
 * two you should change "NR_PORTS" in serial.h to fit your needs.
 *
 * 07/13/95: Initial version - the code is definately _not_ perfect
 *           yet - it doesn't even work :-(
 * 10/01/95: First version, it seems to work pretty well, even at
 *           serial-rates higher than 19200.
 *           There is still one major problem, as I get lotsa overruns
 *           when the harddisk is accessed during receives. This is
 *           probably caused by the SCSI-driver running too long with
 *           interrupts off, so I'm not sure there is anything I can
 *           do about it in here.
 * 10/04/95: Added support for higher (57k6 and 115k2) baud-rates when
 *           the ASYNC_SPD_{HI,VHI} flags are set.
 * 95/10/19: Adapted to 1.2 (Andreas Schwab).
 *	     Compiles, but otherwise untestet!
 * 95/11/26: Updated for 1.2.13pl3 - updated the GVP detection aswell.
 * 96/01/17: Hopefully solved the GVP Zorro detection problem in
 *           1.2.13pl4 that occoured when a user had several GVP
 *           boards installed. (Jes)
 * 96/02/07: The driver actually decreases info->xmit_cnt-- now
 *           whenever a character is transmitted, so we don't just
 *           transmit the same char now. This is the reason why this
 *           driver hasn't worked under 1.2 so far. Thanks to Murray
 *           Bennett for this, found it in his patches.
 * 96/02/27: Integrate various changes to make it work. (murray)
 *           No need to protect against interrupt reentrance.
 *           No need to acknowledge the interrupt (it is done for us).
 *           Process as many conditions as possible during the interrupt.
 *           Use the 16 byte transmit fifo.
 *           Handle THRE interrupts properly. This is a level driven interrupt,
 *           not an edge triggered interrupt, so only enable it when we have
 *           characters to send, and disable it as soon as we have sent the
 *           last character.
 *           Don't try to clear pending interrupts during ioext_init() since
 *           reading IIR achieves nothing.
 *           Register a separate interrupt handler per board. For some
 *           strange reason, I couldn't get it to work with a single handler.
 */

#include <linux/types.h>
#include <linux/sched.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/malloc.h>
#include <linux/termios.h>
#include <linux/tty.h>
#include <linux/serial.h>

#include <asm/setup.h>
#include <asm/irq.h>
#include <asm/amigahw.h>
#include <asm/amigaints.h>
#include <asm/zorro.h>
#include <asm/amigatypes.h>

#include "ser_ioext.h"

#define IOEXT_DEBUG 0 /* Remove this when the driver is finished */
#undef DEBUG_IRQ
#define DEBUG_IRQ 0 /* Remove this when the driver is finished */
#undef DEBUG_IRQ
#define DEBUG 0 /* Remove this when the driver is finished */
#undef DEBUG

#define FIFO_SIZE 16  /* Size of hardware send and receive FIFOs */

#define IOEXT_FAST_RATES 1   /*
			      * When this is set, the rates 110 and
			      * 134 will run 57600 and 115200
			      * respectively.
			      */

#define FIFO_TRIGGER_LEVEL FIFO_TRIG_8
                             /*
			      * There are 4 receiver FIFO-interrupt *
			      * trigger levels (FIFO_TRIG_x), that  *
			      * indicates how many bytes are to be  *
			      * allowed in the receiver-FIFO before *
			      * an interrupt is generated:          *
			      *                x =  1 =  1 byte	    *
			      *                x =  4 =  4 bytes    *
			      *                x =  8 =  8 bytes    *
			      *                x = 14 = 14 bytes    *
			      * 14 works for me, but if you keep    *
			      * getting overruns try lowering this  *
			      * value one step at a time.           *
			      */

/***************************** Prototypes *****************************/
static void ser_init( struct async_struct *info );
static void ser_deinit( struct async_struct *info, int leave_dtr );
static void ser_enab_tx_int( struct async_struct *info, int enab_flag );
static int  ser_check_custom_divisor(struct async_struct *info,
				     int baud_base, int divisor);
static void ser_change_speed( struct async_struct *info );
static void ser_throttle( struct async_struct *info, int status );
static void ser_set_break( struct async_struct *info, int break_flag );
static void ser_get_serial_info( struct async_struct *info,
				struct serial_struct *retinfo );
static unsigned int ser_get_modem_info( struct async_struct *info );
static int ser_set_modem_info( struct async_struct *info, int new_dtr,
			      int new_rts );
static void ser_stop_receive(struct async_struct *info);
static int ser_trans_empty(struct async_struct *info);
/************************* End of Prototypes **************************/

/*
 * SERIALSWITCH structure for the GVP IO-Extender serial-board.
 */

static SERIALSWITCH ioext_ser_switch = {
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

static int ioext_baud_table[18] = {
 	/* B0     */ 0, /* Newer use this value !!! */
	/* B50    */ 9216,
	/* B75    */ 6144,
	/* B110   */ 4189, /* There's a little rounding on this one */
	/* B134	  */ 3439, /* Same here! */
	/* B150	  */ 3072,
	/* B200	  */ 2304,
#if IOEXT_FAST_RATES
	/* B57600 */ 8,
#else
	/* B300	  */ 1536,
#endif
	/* B600	  */ 768,
#if IOEXT_FAST_RATES
	/* B115k2 */ 4,
#else
	/* B1200  */ 384,
#endif
	/* B1800  */ 256,
	/* B2400  */ 192,
	/* B4800  */ 96,
	/* B9600  */ 48,
	/* B19200 */ 24,
	/* B38400 */ 12,   /* The last of the standard rates.  */
	/* B57600 */ 8,    /* ASYNC_SPD_HI                     */
	/* B115K2 */ 4     /* ASYNC_SPD_VHI                    */
};

/* Number of detected ports.  */
static int num_ioext = 0;

/*
 * Functions
 *
 * 'info' points to the first io-extender port.
 *        There are (num_ioext * 2) entries in this array.
 */
static void ser_interrupt(int irq, void *dev_id, struct pt_regs *regs)
{
  struct async_struct *info_table = dev_id;
  u_char i;
  IOEXT_struct *board = (IOEXT_struct *)info_table[0].board_base;

  /* Check if any irq is pending for the current board.            */
  /* Note: The IRQ_PEND-bit=0 if an irq is pending.                */

#ifdef IRQ_DEBUG_2
  printk("ioext: ser_interrupt()\n");
#endif

  if (!(board->CNTR & GVP_IRQ_PEND)) {
#ifdef IRQ_DEBUG_2
    printk("ser_interrupt() - no GVP_IRQ_PEND\n");
#endif
    return;
  }

  for (i = 0; i < 2; i++) {
    u_char iir;
    u_char lsr;
    int ch;
    struct async_struct *info = &info_table[i];
    volatile struct uart_16c550 *uart = curruart(info);

    iir = uart->IIR;

#ifdef IRQ_DEBUG
    printk("ttyS%d: IER=%02X, IIR=%02X, LSR=%02X, MSR=%02X\n", i, uart->IER, iir, uart->LSR, uart->MSR);
#endif

    while (!(iir & IRQ_PEND)) {
      /* IRQ for this uart */
#ifdef IRQ_DEBUG
      printk("IRQ_PEND on ttyS%i...\n", i);
#endif

      switch (iir & (IRQ_ID1 | IRQ_ID2 | IRQ_ID3)) {
        case IRQ_RLS: /* Receiver Line Status */
#ifdef DEBUG_FLOW
        printk("RLS irq on ttyS%i\n", i);
#endif
        case IRQ_CTI: /* Character Timeout */
        case IRQ_RDA: /* Received Data Available */
#ifdef IRQ_DEBUG
        printk("irq (IIR=%02X) on ttyS%i\n", i, iir);
#endif
        /*
         * Copy chars to the tty-queue ...
         * Be careful that we aren't passing one of the
         * Receiver Line Status interrupt-conditions without noticing.
         */
        {
          int got = 0;

          lsr = uart->LSR;
#ifdef IRQ_DEBUG
          printk("uart->LSR & DR = %02X\n", lsr & DR);
#endif
          while (lsr & DR) {
            u_char err = 0;
            ch = uart->RBR;
#ifdef IRQ_DEBUG
            printk("Read a char from the uart: %02X, lsr=%02X\n", ch, lsr);
#endif
            if (lsr & BI) {
              err = TTY_BREAK;
            }
            else if (lsr & PE) {
              err = TTY_PARITY;
            }
            else if (lsr & OE) {
              err = TTY_OVERRUN;
            }
            else if (lsr & FE) {
              err = TTY_FRAME;
            }
#ifdef DEBUG_IRQ
            printk("rs_receive_char(ch=%02X, err=%02X)\n", ch, err);
#endif
            rs_receive_char(info, ch, err);
            got++;
            lsr = uart->LSR;
          }
#ifdef DEBUG_FLOW
          printk("[%d<]", got);
#endif
        }
        break;

      case IRQ_THRE: /* Transmitter holding register empty */
        {
          int fifo_space = 16;
          int sent = 0;

#ifdef IRQ_DEBUG
          printk("THRE-irq for ttyS%i\n", i);
#endif

          /* If the uart is ready to receive data and there are chars in */
          /* the queue we transfer all we can to the uart's FIFO         */
          if (info->xmit_cnt <= 0 || info->tty->stopped ||
              info->tty->hw_stopped) {
            /* Disable transmitter empty interrupt */
            uart->IER &= ~(ETHREI);
            /* Need to send a char to acknowledge the interrupt */
            uart->THR = 0;
#ifdef DEBUG_FLOW
            if (info->tty->hw_stopped) {
              printk("[-]");
            }
            if (info->tty->stopped) {
              printk("[*]");
            }
#endif
            break;
          }

          /* Handle software flow control */
          if (info->x_char) {
#ifdef DEBUG_FLOW
#if 0
            printk("Sending %02X to the uart.\n", info->x_char);
#else
            printk("[^%c]", info->x_char + '@');
#endif
#endif
            uart->THR = info->x_char;
            info->x_char = 0;
            fifo_space--;
            sent++;
          }

          /* Fill the fifo */
          while (fifo_space > 0) {
            fifo_space--;
#ifdef IRQ_DEBUG
            printk("Sending %02x to the uart.\n", info->xmit_buf[info->xmit_tail]);
#endif
            uart->THR = info->xmit_buf[info->xmit_tail++];
            sent++;
            info->xmit_tail = info->xmit_tail & (SERIAL_XMIT_SIZE-1);
            if (--info->xmit_cnt == 0) {
              break;
            }
          }
#ifdef DEBUG_FLOW
            printk("[>%d]", sent);
#endif

          if (info->xmit_cnt == 0) {
#ifdef IRQ_DEBUG
            printk("Sent last char - turning off THRE interrupts\n");
#endif
            /* Don't need THR interrupts any more */
            uart->IER &= ~(ETHREI);
          }

          if (info->xmit_cnt < WAKEUP_CHARS) {
            rs_sched_event(info, RS_EVENT_WRITE_WAKEUP);
          }
        }
        break;

      case IRQ_MS: /* Must be modem status register interrupt? */
        {
          u_char msr = uart->MSR;
#ifdef IRQ_DEBUG
          printk("MS-irq for ttyS%i: %02x\n", i, msr);
#endif

          if (info->flags & ASYNC_INITIALIZED) {
            if (msr & DCTS) {
              rs_check_cts(info, (msr & CTS)); /* active high */
#ifdef DEBUG_FLOW
              printk("[%c-%d]", (msr & CTS) ? '^' : 'v', info->tty ? info->tty->hw_stopped : -1);
#endif
            }
            if (msr & DDCD) {
#ifdef DEBUG
             printk("rs_dcd_changed(%d)\n", !(msr & DCD));
#endif
              rs_dcd_changed(info, !(msr & DCD)); /* active low */
            }
          }
        }
        break;

      default:
#ifdef IRQ_DEBUG
          printk("Unexpected irq for ttyS%i\n", i);
#endif
        break;
      } /* switch (iir) */
      iir = uart->IIR;
    } /* while IRQ_PEND */
  } /* for i = 0, i < 2 */
}


static void ser_init(struct async_struct *info){

#ifdef DEBUG
  printk("ser_init\n");
#endif

  while ((curruart(info)->LSR) & DR){
#if IOEXT_DEBUG
    printk("Emptying uart\n");
#endif
    (void)curruart(info)->RBR;
  }

  /* Set DTR and RTS */
  curruart(info)->MCR |= (DTR | RTS | OUT2);

  /* Enable interrupts. IF_EXTER irq has already been enabled in ioext_init()*/
  /* DON'T enable ETHREI here because there is nothing to send yet (murray) */
  curruart(info)->IER |= (ERDAI | ELSI | EMSI);
}


static void ser_deinit(struct async_struct *info, int leave_dtr){

#ifdef DEBUG
  printk("ser_deinit\n");
#endif

  /* Wait for the uart to get empty */
  while(!(curruart(info)->LSR & TEMT)) {
#if IOEXT_DEBUG
    printk("Waiting for the transmitter to finish\n");
#endif
  }

  while(curruart(info)->LSR & DR) {
#if IOEXT_DEBUG
    printk("Uart not empty - flushing!\n");
#endif
    (void)curruart(info)->RBR;
  }

  /* No need to disable UART interrupts since this will already
   * have been done via ser_enab_tx_int() and ser_stop_receive()
   */

  ser_RTSoff(info);
  if (!leave_dtr)
    ser_DTRoff(info);

}

/*
** ser_enab_tx_int()
** 
** Enable or disable tx interrupts.
** Note that contrary to popular belief, it is not necessary to
** send a character to cause an interrupt to occur. Whenever the
** THR is empty and THRE interrupts are enabled, an interrupt will occur.
** (murray)
*/
static void ser_enab_tx_int(struct async_struct *info, int enab_flag){

  if (enab_flag) {
    curruart(info)->IER |= ETHREI;
  }
  else {
    curruart(info)->IER &= ~(ETHREI);
  }
}

static int  ser_check_custom_divisor(struct async_struct *info,
				     int baud_base, int divisor)
{
  /* Always return 0 or else setserial spd_hi/spd_vhi doesn't work */
  return 0;
}

static void ser_change_speed(struct async_struct *info){

  u_int cflag, baud, chsize, stopb, parity, aflags;
  u_int div = 0, ctrl = 0;

#ifdef DEBUG
  printk("ser_change_speed\n");
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

#ifdef DEBUG
  printk("Changing to baud-rate %i\n", baud);
#endif

  if (baud & CBAUDEX) {
    baud &= ~CBAUDEX;
    if (baud < 1 || baud > 2)
      info->tty->termios->c_cflag &= ~CBAUDEX;
    else
      baud += 15;
  }
  if (baud == 15){
    if (aflags == ASYNC_SPD_HI)   /*  57k6 */
      baud += 1;
    if (aflags == ASYNC_SPD_VHI)  /* 115k2 */
      baud += 2;
    if (aflags == ASYNC_SPD_CUST)
      div = info->custom_divisor;
  }
  if (!div){
      /* Maximum speed is 115200 */
      if (baud > 17) baud = 17;
      div = ioext_baud_table[baud];
  }

  if (!div) {
    /* speed == 0 -> drop DTR */
#ifdef DEBUG
    printk("Dropping DTR\n");
#endif
    ser_DTRoff(info);
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
  ser_DTRon(info);

  if (chsize == CS8){
#ifdef DEBUG
    printk("Setting serial word-length to 8-bits\n");
#endif
    ctrl |= data_8bit;
  } else if (chsize == CS7){
#ifdef DEBUG
    printk("Setting serial word-length to 7-bits\n");
#endif
    ctrl |= data_7bit;
  } else if (chsize == CS6){
#ifdef DEBUG
    printk("Setting serial word-length to 6-bits\n");
#endif
    ctrl |= data_6bit;
  } else if (chsize == CS5){
#ifdef DEBUG
    printk("Setting serial word-length to 5-bits\n");
#endif
    ctrl |= data_5bit;
   };


  /* If stopb is true we set STB which means 2 stop-bits - */
  /* otherwise we  only get 1 stop-bit.                    */
  ctrl |= (stopb ? STB : 0);
     
  ctrl |= ((parity & PARENB) ? ((parity & PARODD) ? (PEN) : (PEN |
							     EPS)) :
	   0x00 ); 

#ifdef DEBUG
  printk ("Storing serial-divisor %i\n", div);
#endif

  curruart(info)->LCR = (ctrl | DLAB);

  /* Store high byte of divisor */
  curruart(info)->DLM = ((div >> 8) & 0xff);
  
  /* Store low byte of divisor */

  curruart(info)->DLL = (div & 0xff);

  curruart(info)->LCR = ctrl;
}


static void ser_throttle(struct async_struct *info, int status){

#ifdef DEBUG
  printk("ser_throttle\n");
#endif
  if (status){
    ser_RTSoff(info);
  }
  else{
    ser_RTSon(info);
  }
}


static void ser_set_break(struct async_struct *info, int break_flag){

#if IOEXT_DEBUG
  printk("ser_set_break\n");
#endif
    if (break_flag)
	curruart(info)->LCR |= SET_BREAK;
    else
	curruart(info)->LCR &= ~SET_BREAK;
}


static void ser_get_serial_info(struct async_struct *info,
				struct serial_struct *retinfo){
#ifdef DEBUG
  printk("ser_get_serial_info\n");
#endif


  retinfo->baud_base = IOEXT_BAUD_BASE;
  retinfo->xmit_fifo_size = FIFO_SIZE; /* This field is currently ignored, */
				/* by the upper layers of the       */
				/* serial-driver.                   */
  retinfo->custom_divisor = info->custom_divisor;

}

static unsigned int ser_get_modem_info(struct async_struct *info){

  u_char msr, mcr;

#ifdef DEBUG
  printk("ser_get_modem_info\n");
#endif

  msr = curruart(info)->MSR;
  mcr = curruart(info)->MCR; /* The DTR and RTS are located in the */
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

static int ser_set_modem_info(struct async_struct *info, int new_dtr,
			      int new_rts){
#ifdef DEBUG
  printk("ser_set_modem_info new_dtr=%i new_rts=%i\n", new_dtr, new_rts);
#endif
  if (new_dtr == 0)
    ser_DTRoff(info);
  else if (new_dtr == 1)
    ser_DTRon(info);

  if (new_rts == 0){
    ser_RTSoff(info);
  }else{
    if (new_rts == 1){
      ser_RTSon(info);
    }
  }

  return 0;
};

static void ser_stop_receive (struct async_struct *info)
{
  /* Disable uart receive and status interrupts */
  curruart(info)->IER &= ~(ERDAI | ELSI | EMSI);
}

static int ser_trans_empty (struct async_struct *info)
{
  return (curruart(info)->LSR & THRE);
}

/*
 * Detect and initialize all IO-Extenders in this system.
 */
int ioext_init(void)
{
  int key = 0;
  struct ConfigDev *cd;
  caddr_t address;
  enum GVP_ident epc;
  short i;
  int line1, line2;
  struct serial_struct req;
  u_char isr_installed = 0;
  IOEXT_struct *board;                  /* Used as a pointer to the    */
				        /* board-address               */
  IOEXT_struct *ioext_bases[MAX_IOEXT]; /* Just used to inform the     */
					/* user of the location of the */
					/* boards found during         */
					/* configuration.              */     

  if (!MACH_IS_AMIGA)
      return -ENODEV;

  for (i=0 ; i < MAX_IOEXT; i++)
    ioext_bases[i] = NULL;

  /*
   * key++ is not needed anymore when a board is found, as zorro_find
   * returns board_nr + 1 and it adds 1 to the index before it starts
   * searching.
   */
  while ((key = zorro_find(MANUF_GVP, PROD_GVP, 0, key))) {

    cd = zorro_get_board(key);

    /* As GVP uses the same product code for several kind of boards, */
    /* we have to check the extended product code, to see if this */
    /* really is an io-extender. */
    /* check extended product code */
    address = cd->cd_BoardAddr;
    epc = *(enum GVP_ident *)ZTWO_VADDR(address + 0x8000) & GVP_PRODMASK;

    if (epc != GVP_IOEXT){
      continue;
    }

    /* Wait for the board to be ready and disable everything */
    board = (IOEXT_struct *) ZTWO_VADDR(address);

    ioext_bases[num_ioext] = board;

    while(!board->CNTR & GVP_IRQ_PEND) {
	printk("GVP-irq pending \n");
    }
      
    /*
     * Disable board-interrupts for the moment.
     */
    board->CNTR = 0;

    /*
     * Setting the necessary tty-stuff.
     */
    req.line              = -1; /* first free ttyS? device */
    req.type              = SER_IOEXT;
    req.port              = (int) &board->uart0;
    if ((line1 = register_serial( &req )) < 0) {
	printk( "Cannot register IO-Extender serial port: no free device\n" );
	return -EBUSY;
    }
    rs_table[line1].nr_uarts = 0;
    rs_table[line1].board_base = board;
    rs_table[line1].sw = &ioext_ser_switch;

    req.line              = -1; /* first free ttyS? device */
    req.type              = SER_IOEXT;
    req.port              = (int) &board->uart1;
    if ((line2 = register_serial( &req )) < 0) {
	printk( "Cannot register IO-Extender serial port: no free device\n" );
	unregister_serial( line1 );
	return -EBUSY;
    }
    rs_table[line2].nr_uarts = 1;
    rs_table[line2].board_base = board;
    rs_table[line2].sw = &ioext_ser_switch;

    /* Install ISR if it hasn't been installed already */
    if (!isr_installed) {
	request_irq(IRQ_AMIGA_EXTER, ser_interrupt, 0,
	            "io-extender serial", rs_table + line1);
	isr_installed++;
    }

    /*
     * Set the board to INT6 and RTSx port-control.
     * (GVP uses this setup).
     */
    board->CTRL = (IRQ_SEL|PORT0_CTRL|PORT1_CTRL);

    /* Add {} in here so that without debugging we still get the
     * desired effect (murray)
     */

    /* Wait for the uarts to get empty */
    while(!(board->uart0.LSR & TEMT)) {
#if IOEXT_DEBUG
	printk("Waiting for transmitter 0 to finish\n");
#endif
    }

    /* Wait for the uart to get empty */
    while(!(board->uart1.LSR & TEMT)) {
#if IOEXT_DEBUG
	printk("Waiting for transmitter 1 to finish\n");
#endif
    }

    /*
     * Set the uarts to a default setting og 8N1 - 9600
     */

    board->uart0.LCR = (data_8bit | DLAB);
    board->uart0.DLM = 0;
    board->uart0.DLL = 48;
    board->uart0.LCR = (data_8bit);

    board->uart1.LCR = (data_8bit | DLAB);
    board->uart1.DLM = 0;
    board->uart1.DLL = 48;
    board->uart1.LCR = (data_8bit);


    /*
     * Enable + reset both the tx and rx FIFO's.
     * Sett the rx FIFO-trigger count.
     */

    board->uart0.FCR =  ( FIFO_ENA      | RCVR_FIFO_RES |
		          XMIT_FIFO_RES | FIFO_TRIGGER_LEVEL );

    board->uart1.FCR =  ( FIFO_ENA      | RCVR_FIFO_RES |
			  XMIT_FIFO_RES | FIFO_TRIGGER_LEVEL );

    /* 
     * Initialise parallel port.
     */
    board->par.CTRL = (INT2EN | SLIN | INIT);

    /*
     * Master interrupt enable - National semconductors doesn't like
     * to follow standards, so their version of the chip is
     * different and I only had a copy of their data-sheets :-(
     */
    board->uart0.MCR = OUT2;
    board->uart1.MCR = OUT2;

    /*
     * Disable all uart interrups (they will be re-enabled in
     * ser_init when they are needed).
     */
    board->uart0.IER = 0;
    board->uart1.IER = 0;


    num_ioext++;
    zorro_config_board(key, 0);

    /*
     * Re-enable all IRQ's for the current board (board interrupt).
     */
    board->CNTR |= GVP_IRQ_ENA;
  }

  /* Print number of IO-Extenders found and their base-addresses.*/
  if (num_ioext > 0){
    printk ("Detected %i GVP IO-Extender(s) at: ", num_ioext);
    for (i = 0 ; i < num_ioext ; i++){
      printk ("0x%08x", (unsigned int) ioext_bases[i]);
      if (i+1 < num_ioext)
	printk(", ");
    }
    printk("\n");
  }

  return(0);
}
