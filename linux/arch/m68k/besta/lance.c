/*
 * besta/lance.c -- Low level network driver for LAN VME-board.
 *
 * Written 1996, 1997	    Dmitry K. Butskoy
 *			    <buc@citadel.stu.neva.ru>
 *
 *
 * This code is adapted by `drivers/net/atarilance.c' as most relative.
 *
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2, or (at your option) any
 * later version.
 *
 */

#include <linux/stddef.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/ptrace.h>
#include <linux/errno.h>
#include <linux/malloc.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/interrupt.h>

#include <asm/setup.h>
#include <asm/traps.h>

#include "besta.h"

/*  These define the number of Rx and Tx buffers as log2. (Only powers
   of two are valid).
    Much more rx buffers (32) are reserved than tx buffers (8), since receiving
   is more time critical then sending and packets may have to remain in the
   board's memory when main memory is low.
*/

#define TX_LOG_RING_SIZE        3
#define RX_LOG_RING_SIZE        5

/* These are the derived values */

#define TX_RING_SIZE            (1 << TX_LOG_RING_SIZE)
#define TX_RING_LEN_BITS        (TX_LOG_RING_SIZE << 5)
#define TX_RING_MOD_MASK        (TX_RING_SIZE - 1)

#define RX_RING_SIZE            (1 << RX_LOG_RING_SIZE)
#define RX_RING_LEN_BITS        (RX_LOG_RING_SIZE << 5)
#define RX_RING_MOD_MASK        (RX_RING_SIZE - 1)


struct odd {
	char          r0;
	unsigned char reg;
};

struct ring {
	unsigned short base;    /*  low word of base address   */
	unsigned char  flag;
	unsigned char  base_hi; /*  high bits of base address (unused)   */
	short          length;  /*  2s complement length   */
	short          param;   /*  Rx: msg length, Tx: misc   */
};

struct lance {
	unsigned short data;            /*  register data port   */
	unsigned short addr;            /*  register address port   */
	unsigned short level;           /*  interrupt level   */
	unsigned short vector;          /*  interrupt vector   */
	short          filler0[4];
	struct odd init_ethaddr[8];     /*  really 6 in use   */

	short          filler_offset[(0x10000 - 32) / 2];

	/*  initialization block   */
	unsigned short mode;            /*  pre-set mode   */
	unsigned char  hwaddr[6];       /*  word-little-endian
					    physical ethernet address   */
	unsigned long  filter[2];       /*  multicast filter (not used ?)  */
	unsigned short rx_ring_addr;    /*  low 16 bits of address   */
	unsigned char  rx_ring_len;     /*  length bits   */
	unsigned char  rx_ring_addr_hi; /*  high 8 bits of address (unused)  */
	unsigned short tx_ring_addr;    /*  low 16 bits of address   */
	unsigned char  tx_ring_len;     /*  length bits   */
	unsigned char  tx_ring_addr_hi; /*  high 8 bits of address (unused)  */

	struct ring tx_ring[TX_RING_SIZE];
	struct ring rx_ring[RX_RING_SIZE];

	char memory[0];         /*  ether packet data follow after
				   the init block and the ring descriptors
				   and are located at runtime   */
};


/* The driver's private device structure */
struct lance_private {
	unsigned int board_addr;
	int     cur_rx;         /*  the next free ring entry   */
	int     cur_tx;         /*  the next free ring entry   */
	int     dirty_tx;       /*  ring entries to be freed   */
	int     tx_full;        /*  should be atomic...  */
	int     lock;           /*  should be atomic...  */
	struct enet_statistics stats;
};


#define PKT_BUF_SZ              1544


/*      Various flags definition...   */

/* tx_head flags */
#define TMD1_ENP        0x01    /* end of packet */
#define TMD1_STP        0x02    /* start of packet */
#define TMD1_DEF        0x04    /* deferred */
#define TMD1_ONE        0x08    /* one retry needed */
#define TMD1_MORE       0x10    /* more than one retry needed */
#define TMD1_ERR        0x40    /* error summary */
#define TMD1_OWN        0x80    /* ownership (set: chip owns) */

#define TMD1_OWN_CHIP   TMD1_OWN
#define TMD1_OWN_HOST   0

/* tx_head misc field */
#define TMD3_TDR        0x03FF  /* Time Domain Reflectometry counter */
#define TMD3_RTRY       0x0400  /* failed after 16 retries */
#define TMD3_LCAR       0x0800  /* carrier lost */
#define TMD3_LCOL       0x1000  /* late collision */
#define TMD3_UFLO       0x4000  /* underflow (late memory) */
#define TMD3_BUFF       0x8000  /* buffering error (no ENP) */

/* rx_head flags */
#define RMD1_ENP        0x01    /* end of packet */
#define RMD1_STP        0x02    /* start of packet */
#define RMD1_BUFF       0x04    /* buffer error */
#define RMD1_CRC        0x08    /* CRC error */
#define RMD1_OFLO       0x10    /* overflow */
#define RMD1_FRAM       0x20    /* framing error */
#define RMD1_ERR        0x40    /* error summary */
#define RMD1_OWN        0x80    /* ownership (set: ship owns) */

#define RMD1_OWN_CHIP   RMD1_OWN
#define RMD1_OWN_HOST   0

/* register names */
#define CSR0    0       /* mode/status */
#define CSR1    1       /* init block addr (low) */
#define CSR2    2       /* init block addr (high) */
#define CSR3    3       /* misc */
#define CSR8    8       /* address filter */
#define CSR15   15      /* promiscuous mode */

/* CSR0 */
/* (R=readable, W=writeable, S=set on write, C=clear on write) */
#define CSR0_INIT       0x0001          /* initialize (RS) */
#define CSR0_STRT       0x0002          /* start (RS) */
#define CSR0_STOP       0x0004          /* stop (RS) */
#define CSR0_TDMD       0x0008          /* transmit demand (RS) */
#define CSR0_TXON       0x0010          /* transmitter on (R) */
#define CSR0_RXON       0x0020          /* receiver on (R) */
#define CSR0_INEA       0x0040          /* interrupt enable (RW) */
#define CSR0_INTR       0x0080          /* interrupt active (R) */
#define CSR0_IDON       0x0100          /* initialization done (RC) */
#define CSR0_TINT       0x0200          /* transmitter interrupt (RC) */
#define CSR0_RINT       0x0400          /* receiver interrupt (RC) */
#define CSR0_MERR       0x0800          /* memory error (RC) */
#define CSR0_MISS       0x1000          /* missed frame (RC) */
#define CSR0_CERR       0x2000          /* carrier error (no heartbeat :-) (RC) */
#define CSR0_BABL       0x4000          /* babble: tx-ed too many bits (RC) */
#define CSR0_ERR        0x8000          /* error (RC) */

/* CSR3 */
#define CSR3_BCON       0x0001          /* byte control */
#define CSR3_ACON       0x0002          /* ALE control */
#define CSR3_BSWP       0x0004          /* byte swap (1=big endian) */


static int lance_open (struct device *dev);
static int lance_start_xmit (struct sk_buff *skb, struct device *dev);
static void lance_intr (int vec, void *data, struct pt_regs *fp);
static int lance_close (struct device *dev);
static struct enet_statistics *lance_get_stats (struct device *dev);
static void lance_set_multicast_list (struct device *dev);
static int do_lance_init (struct device *);


void lance_init (struct VME_board *VME, int on_off) {
	volatile struct lance *lance = (struct lance *) VME->addr;
	struct lance_private *lp;
	struct device *lance_dev;
	int vector, i;

	if (on_off) {
	    unsigned int i;

	    i = CSR0;
	    if (VME_probe (&lance->addr, &i, PROBE_WRITE, PORT_WORD))
		    return;     /*  board is located or runs away...  */

	    lance->addr = CSR0;
	    lance->data = CSR0_STOP;    /* 0x4  */

	    lance->mode = 0x0000;       /*  dis Tx, dis Rx   */


	    lance->level = 0;
	    lance->vector = VEC_UNINT;

	    return;
	}


	if (VME_probe (&lance->mode, 0, PROBE_READ, PORT_BYTE)) {
	    printk ("    no %s at 0x%08x\n",
		    (VME->name ? VME->name : "board"), VME->addr);
	    return;
	}

	printk ("  0x%08x: LANCE Am7990 board, ethernet address "
		"%02x:%02x:%02x:%02x:%02x:%02x\n",
		VME->addr,
		lance->init_ethaddr[0].reg,
		lance->init_ethaddr[1].reg,
		lance->init_ethaddr[2].reg,
		lance->init_ethaddr[3].reg,
		lance->init_ethaddr[4].reg,
		lance->init_ethaddr[5].reg
	);

	vector = (VME->vect < 0) ? get_unused_vector() : VME->vect;

	lance_dev = (struct device *) kmalloc (sizeof (*lance_dev), GFP_KERNEL);
	lp = (struct lance_private *) kmalloc (sizeof (*lp), GFP_KERNEL);
	if (!lance_dev || !lp) {
		printk ("lance_init: cannot malloc buffers\n");
		return;
	}

	VME->present = 1;       /*  OK, board is present   */

	memset (lance_dev, 0, sizeof (*lance_dev));
	memset (lp, 0, sizeof (*lp));

	/*  the name field will be generated autimatically
	   depended by ethernet controllers number...   ("eth0", "eth1", etc)
	*/
	lance_dev->name = ((char []) { 0,0,0,0,0,0,0,0 });  /*  8 bytes area */
	lance_dev->init = do_lance_init;
	lance_dev->priv = lp;    /*  early initialization   */
	lance_dev->base_addr = VME->addr;
	lp->board_addr = VME->addr;


	/*  Add `lance_dev' to the end of `dev_base' chain.
	    Then it will be initialized by net_dev_init() .    */

	if (dev_base == NULL)  dev_base = lance_dev;
	else {
	    struct device *dev = dev_base;

	    while (dev->next)  dev = dev->next;
	    dev->next = lance_dev;
	}
	lance_dev->next = NULL;


	/*  do hardware initialization stuff hear   */
	lance->addr = CSR0;
	lance->data = CSR0_STOP;    /* 0x4  */

	lance->mode = 0x0000;       /*  dis Tx, dis Rx   */

	for (i = 0; i < 6; i++)
		lance_dev->dev_addr[i] = lance->init_ethaddr[i].reg;
	for (i = 0; i < 6; i++)
		lance->hwaddr[i] = lance_dev->dev_addr[i^1];  /* be --> le  */

	lance->filter[0] = 0x00000000;
	lance->filter[1] = 0x00000000;
	lance->rx_ring_addr = offsetof (struct lance, rx_ring) -
					offsetof (struct lance, mode);
	lance->rx_ring_addr_hi = 0;
	lance->rx_ring_len = RX_RING_LEN_BITS;
	lance->tx_ring_addr = offsetof (struct lance, tx_ring) -
					offsetof (struct lance, mode);
	lance->tx_ring_addr_hi = 0;
	lance->tx_ring_len = TX_RING_LEN_BITS;

	lance->level = VME->lev;
	lance->vector = vector;

	lance_dev->irq = vector;     /*  for later initialization   */

	return;
}

static int do_lance_init (struct device *dev) {
	struct lance_private *lp = dev->priv;   /*  is already initialized   */

	if (!lp || !dev->irq)  return -ENODEV;

	init_etherdev (dev, 0);

	dev->open = &lance_open;
	dev->hard_start_xmit = &lance_start_xmit;
	dev->stop = &lance_close;
	dev->get_stats = &lance_get_stats;
	dev->set_multicast_list = &lance_set_multicast_list;
	dev->start = 0;

	memset (&lp->stats, 0, sizeof (lp->stats));

	besta_handlers[dev->irq] = lance_intr;
	besta_intr_data[dev->irq] = dev;

	printk ("lance netdevice registered (%s)\n", dev->name);

	return 0;
}


/*  Initialize the LANCE Rx and Tx rings.  */

static void lance_init_ring (struct device *dev) {
	struct lance_private *lp = dev->priv;
	volatile struct lance *lance = (struct lance *) dev->base_addr;
	int i;
	unsigned offset;

	lp->lock = 0;
	lp->tx_full = 0;
	lp->cur_rx = lp->cur_tx = 0;
	lp->dirty_tx = 0;

	offset = offsetof (struct lance, memory) -
				offsetof (struct lance, mode);

	for (i = 0; i < TX_RING_SIZE; i++) {
	    lance->tx_ring[i].base = offset;
	    lance->tx_ring[i].flag = TMD1_OWN_HOST;     /* 0x0  */
	    lance->tx_ring[i].base_hi = 0;
	    lance->tx_ring[i].length = 0;
	    lance->tx_ring[i].param = 0;

	    offset += PKT_BUF_SZ;
	}

	for (i = 0; i < RX_RING_SIZE; i++) {
	    lance->rx_ring[i].base = offset;
	    lance->rx_ring[i].flag = TMD1_OWN_CHIP;     /* 0x80  */
	    lance->rx_ring[i].base_hi = 0;
	    lance->rx_ring[i].length = -PKT_BUF_SZ;
	    lance->rx_ring[i].param = 0;

	    offset += PKT_BUF_SZ;
	}

	return;
}


static int lance_open (struct device *dev) {
	volatile struct lance *lance = (struct lance *) dev->base_addr;
	int i;

	lance_init_ring(dev);

	/*  Re-initialize the LANCE, and start it when done.  */

	lance->addr = CSR3;
	lance->data = CSR3_BSWP;        /*  0x0004   */
	lance->addr = CSR2;
	lance->data = 0;
	lance->addr = CSR1;
	lance->data = 0;
	lance->addr = CSR0;
	lance->data = CSR0_INIT;        /*  0x0001   */
	/*  from now on, lance->addr is kept to point to CSR0   */

	for (i = 1000000; i && !(lance->data & CSR0_IDON); i--) ;  /* 0x0100 */

	if (i <= 0 || (lance->data & CSR0_ERR)) {       /*  0x8000   */
	    printk ("lance: opening %s failed, csr0=%04x\n",
					    dev->name, lance->data);

	    lance->data = CSR0_STOP;    /*  0x0004   */
	    return -EIO;
	}

	lance->data = CSR0_IDON;        /*  0x0100   */
	lance->data = CSR0_STRT;        /*  0x0002   */
	lance->data = CSR0_INEA;        /*  0x0040   */

	dev->tbusy = 0;
	dev->interrupt = 0;
	dev->start = 1;

	/*   MOD_INC_USE_COUNT  should be placed hear...  */

	return 0;
}

static int lance_close (struct device *dev) {
	volatile struct lance *lance = (struct lance *) dev->base_addr;

	dev->start = 0;
	dev->tbusy = 1;

	/*  We stop the LANCE here -- it occasionally polls
	   memory if we don't.
	*/
	lance->addr = CSR0;
	lance->data = CSR0_STOP;        /*  0x0004   */

	/*   MOD_DEC_USE_COUNT  should be placed hear...  */

	return 0;
}


static struct enet_statistics *lance_get_stats (struct device *dev) {
	struct lance_private *lp = dev->priv;

	return &lp->stats;
}


static int lance_start_xmit (struct sk_buff *skb, struct device *dev) {
	volatile struct lance *lance = (struct lance *) dev->base_addr;
	struct lance_private *lp = dev->priv;
	int entry, len;
	volatile struct ring *head;
	unsigned short flags;

	/*  Transmitter timeout, serious problems.  */
	if (dev->tbusy) {
	    int tickssofar = jiffies - dev->trans_start;

	    if (tickssofar < 20)  return 1;

	    lance->addr = CSR0;
	    printk ("lance: %s: transmit timed out, status %04x, resetting.\n",
						    dev->name, lance->data);
	    lance->data = CSR0_STOP;    /*  0x0004   */

	    /*  Always set BSWP after a STOP as STOP puts it back into
	       little-endian mode.
	    */
	    lance->addr = CSR3;
	    lance->data = CSR3_BSWP;    /*  0x0004   */

	    lp->stats.tx_errors++;

	    lance_init_ring (dev);

	    lance->addr = CSR0;
	    lance->data = CSR0_INEA | CSR0_INIT | CSR0_STRT;

	    dev->tbusy = 0;
	    dev->trans_start = jiffies;

	    return 0;
	}

	if (skb == NULL) {
		dev_tint (dev);
		return 0;
	}

	if (skb->len <= 0)  return 0;


	/* Block a timer-based transmit from overlapping.  This could better be
	   done with atomic_swap(1, dev->tbusy), but set_bit() works as well. */
	if (set_bit (0, (void*) &dev->tbusy) != 0) {
	    printk ("lance: %s: Transmitter access conflict.\n", dev->name);
	    return 1;
	}

	if (set_bit (0, (void*) &lp->lock) != 0) {
		printk ("lance: %s: tx queue lock!.\n", dev->name);
		/* don't clear dev->tbusy flag. */
		return 1;
	}

	/*  We're not prepared for the int until the last flags are set/reset.
	   And the int may happen already after setting the OWN_CHIP...
	*/
	save_flags(flags);
	cli();

	/*  Mask to ring buffer boundary.  */
	entry = lp->cur_tx & TX_RING_MOD_MASK;
	head = &lance->tx_ring[entry];

	/*  Caution: the write order is important here,
	   set the "ownership" bits last.
	*/

	len = (skb->len > ETH_ZLEN) ? skb->len : ETH_ZLEN;

	head->length = -len;
	head->param = 0;
	memcpy ((char *) &lance->mode + head->base, skb->data, skb->len);
	head->flag = TMD1_OWN_CHIP | TMD1_ENP | TMD1_STP;

	dev_kfree_skb (skb, FREE_WRITE);

	lp->cur_tx++;

	while (lp->cur_tx >= TX_RING_SIZE && lp->dirty_tx >= TX_RING_SIZE) {
		lp->cur_tx -= TX_RING_SIZE;
		lp->dirty_tx -= TX_RING_SIZE;
	}
	/*  it was parrranoida-a-al ...   */

	/* Trigger an immediate send poll. */
	lance->data = CSR0_INEA | CSR0_TDMD;    /*  0x0040 | 0x0008  */

	dev->trans_start = jiffies;

	lp->lock = 0;
	if ((lance->tx_ring[(entry+1) & TX_RING_MOD_MASK].flag & TMD1_OWN) ==
								TMD1_OWN_HOST)
	    dev->tbusy = 0;
	else
	    lp->tx_full = 1;

	restore_flags(flags);
	return 0;
}


static void lance_intr (int vec, void *data, struct pt_regs *fp) {
	struct device *dev = data;
	volatile struct lance *lance = (struct lance *) dev->base_addr;
	struct lance_private *lp = dev->priv;
	int csr0, i, boguscnt = 10;


	if (dev == NULL) {
		printk ("lance_intr: unknown device\n");
		return;
	}

	if (dev->interrupt)
		printk ("lance_intr: re-entering the interrupt handler\n");

	dev->interrupt = 1;


	lance->addr = CSR0;

	while (((csr0 = lance->data) & (CSR0_ERR | CSR0_TINT | CSR0_RINT)) &&
	       --boguscnt >= 0
	) {

	    /*  Acknowledge all of the current interrupt sources ASAP.   */
	    lance->data = csr0 &
		~(CSR0_INIT | CSR0_STRT | CSR0_STOP | CSR0_TDMD | CSR0_INEA);


	    /*   Rx  interrupt stuff...  */

	    if (csr0 & CSR0_RINT) {     /*  Rx interrupt   */
		int entry = lp->cur_rx & RX_RING_MOD_MASK;

		/*  If we own the next entry, it's a new packet. Send it up. */
		while ((lance->rx_ring[entry].flag & RMD1_OWN) ==
							RMD1_OWN_HOST) {
		    volatile struct ring *head = &lance->rx_ring[entry];
		    int status = head->flag;

		    if (status != (RMD1_ENP | RMD1_STP)) {
			/*  there was an error   */

			if (status & RMD1_ENP)
				lp->stats.rx_errors++;
			if (status & RMD1_FRAM)
				lp->stats.rx_frame_errors++;
			if (status & RMD1_OFLO)
				lp->stats.rx_over_errors++;
			if (status & RMD1_CRC)
				lp->stats.rx_crc_errors++;
			if (status & RMD1_BUFF)
				lp->stats.rx_fifo_errors++;

			head->flag &= (RMD1_ENP | RMD1_STP);

		    } else {
			/*  Malloc up new buffer, compatible with net-3.  */
			short pkt_len = head->param & 0xfff;
			struct sk_buff *skb;

			if (pkt_len < ETH_ZLEN)
				lp->stats.rx_errors++;
			else {
			    skb = dev_alloc_skb (pkt_len + 2);

			    if (skb == NULL) {
				printk ("lance: %s: Memory squeeze, "
					"deferring packet.\n", dev->name);

				for (i = 0; i < RX_RING_SIZE; i++) {
				    int j = (entry + i) & RX_RING_MOD_MASK;

				    if (lance->rx_ring[j].flag & RMD1_OWN_CHIP)
					    break;
				}

				if (i > RX_RING_SIZE - 2) {
				    lp->stats.rx_dropped++;
				    head->flag |= RMD1_OWN_CHIP;
				    lp->cur_rx++;
				}
				break;
			    }

			    skb->dev = dev;
			    skb_reserve (skb, 2);   /*  16 byte align   */
			    skb_put (skb, pkt_len);     /*  Make room   */

			    memcpy (skb->data,
					(char *) &lance->mode + head->base,
					    pkt_len);

			    skb->protocol = eth_type_trans (skb, dev);

			    netif_rx (skb);

			    lp->stats.rx_packets++;
			}
		    }

		    head->flag |= RMD1_OWN_CHIP;
		    entry = (++lp->cur_rx) & RX_RING_MOD_MASK;

		}   /*  while ( ... )   */

		lp->cur_rx &= RX_RING_MOD_MASK;
	    }


	    /*   Tx  interrupt stuff...  */

	    if (csr0 & CSR0_TINT) {     /*  Tx interrupt   */
		int dirty_tx = lp->dirty_tx;

		while (dirty_tx < lp->cur_tx) {
		    int entry = dirty_tx & TX_RING_MOD_MASK;
		    int status = lance->tx_ring[entry].flag;

		    if (status & TMD1_OWN_CHIP)  break;
					/*  It still hasn't been Txed   */

		    lance->tx_ring[entry].flag = 0;

		    if (status & TMD1_ERR) {
			/* There was an major error, log it. */
			int err_status = lance->tx_ring[entry].param;

			lp->stats.tx_errors++;
			if (err_status & TMD3_RTRY)
				lp->stats.tx_aborted_errors++;
			if (err_status & TMD3_LCAR)
				lp->stats.tx_carrier_errors++;
			if (err_status & TMD3_LCOL)
				lp->stats.tx_window_errors++;
			if (err_status & TMD3_UFLO) {
			    /*  On FIFO errors the Tx unit is turned off!  */

			    lp->stats.tx_fifo_errors++;
			    /* Remove this verbosity later! */
			    printk ("lance: %s: Tx FIFO error! Status %04x\n",
							    dev->name, csr0);
			    /* Restart the chip. */
			    lance->data = CSR0_STRT;    /*  0x0002   */
			}

		    } else {
			if (status & (TMD1_MORE | TMD1_ONE | TMD1_DEF))
				lp->stats.collisions++;

			lp->stats.tx_packets++;
		    }

		    dirty_tx++;

		}   /*  while (dirty_tx < lp->cur_tx) ...  */


		if (lp->tx_full &&
		    dev->tbusy &&
		    dirty_tx > lp->cur_tx - TX_RING_SIZE + 2
		) {
		    /* The ring is no longer full, clear tbusy. */
		    lp->tx_full = 0;
		    dev->tbusy = 0;
		    mark_bh (NET_BH);
		}

		lp->dirty_tx = dirty_tx;
	    }


	    /*  log misc errors...  */

	    if (csr0 & CSR0_BABL)
		    lp->stats.tx_errors++;      /*  Tx babble.  */
	    if (csr0 & CSR0_MISS)
		    lp->stats.rx_errors++;      /*  Missed a Rx frame.  */
	    if (csr0 & CSR0_MERR) {
		printk ("lance: %s: Bus master arbitration failure (?!?), "
			"status %04x.\n", dev->name, csr0);
		/* Restart the chip. */
		lance->data = CSR0_STRT;        /*  0x0002   */
	    }

	}  /*  while ( ... )   */


	/* Clear any other interrupt, and set interrupt enable. */
	lance->data = CSR0_BABL | CSR0_CERR | CSR0_MISS | CSR0_MERR |
						    CSR0_IDON | CSR0_INEA;

	dev->interrupt = 0;

	return;
}


/* Set or clear the multicast filter for this adaptor.
   num_addrs == -1      Promiscuous mode, receive all packets
   num_addrs == 0       Normal mode, clear multicast list
   num_addrs > 0        Multicast mode, receive normal and MC packets, and do
							best-effort filtering.
*/

static void lance_set_multicast_list (struct device *dev) {
	volatile struct lance *lance = (struct lance *) dev->base_addr;

	/* Only possible if board is already started */
	if (!dev->start)  return;

	/*  We take the simple way out and always enable promiscuous mode.  */
	lance->data = CSR0_STOP;    /* Temporarily stop the lance. */

	if (dev->flags & IFF_PROMISC) {
	    /*  Log any net taps.  */
	    lance->addr = CSR15;
	    lance->data = 0x8000;       /* Set promiscuous mode */

	} else {
	    short multicast_table[4];
	    int num_addrs = dev->mc_count;
	    int i;

	    /*  We don't use the multicast table,
	      but rely on upper-layer filtering.
	    */
	    memset (multicast_table, (num_addrs == 0) ? 0 : -1,
					    sizeof (multicast_table));

	    for (i = 0; i < 4; i++) {
		    lance->addr = CSR8 + i;
		    lance->data = multicast_table[i];
	    }

	    lance->addr = CSR15;
	    lance->data = 0;    /* Unset promiscuous mode */
	}

	/*  Always set BSWP after a STOP as STOP puts it back into
	  little endian mode.
	*/
	lance->addr = CSR3;
	lance->data = CSR3_BSWP;

	/*  Resume normal operation and reset AREG to CSR0   */
	lance->addr = CSR0;
	lance->data = CSR0_IDON | CSR0_INEA | CSR0_STRT;

	return;
}

