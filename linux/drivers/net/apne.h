
/*
 * Amiga Linux/68k 8390 based PCMCIA Ethernet Driver for the Amiga 1200
 *
 * (C) Copyright 1997 Alain Malek
 *                    (Alain.Malek@cryogen.com)
 *
 * ----------------------------------------------------------------------------
 *
 * This program is based on
 *
 * ne.c:       A general non-shared-memory NS8390 ethernet driver for linux
 *             Written 1992-94 by Donald Becker.
 *
 * 8390.c:     A general NS8390 ethernet driver core for linux.
 *             Written 1992-94 by Donald Becker.
 *
 * cnetdevice: A Sana-II ethernet driver for AmigaOS
 *             Written by Bruce Abbott (bhabbott@inhb.co.nz)
 *
 * ----------------------------------------------------------------------------
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file COPYING in the main directory of the Linux
 * distribution for more details.
 *
 * ----------------------------------------------------------------------------
 *
 */

#ifndef _apne_h
#define _apne_h

#include <linux/if_ether.h>
#include <linux/ioport.h>
#include <linux/skbuff.h>


/* nex000 registers */

#define NE_BASE	 (dev->base_addr)
#define NE_CMD	 	0x00
#define NE_DATAPORT	0x10		/* NatSemi-defined port window offset. */
#define NE_RESET	0x1f	 	/* Issue a read to reset, a write to clear. */
#define NE_IO_EXTENT	0x20

#define NE1SM_START_PG	0x20	/* First page of TX buffer */
#define NE1SM_STOP_PG 	0x40	/* Last page +1 of RX ring */
#define NESM_START_PG	0x40	/* First page of TX buffer */
#define NESM_STOP_PG	0x80	/* Last page +1 of RX ring */

/* 8390 registers */

/* Some generic ethernet register configurations. */
#define E8390_TX_IRQ_MASK 0xa		/* For register EN0_ISR */
#define E8390_RX_IRQ_MASK  0x5
#define E8390_RXCONFIG 0x4		/* EN0_RXCR: broadcasts, no multicast,errors */
#define E8390_RXOFF 0x20		/* EN0_RXCR: Accept no packets */
#define E8390_TXCONFIG 0x00		/* EN0_TXCR: Normal transmit mode */
#define E8390_TXOFF 0x02		/* EN0_TXCR: Transmitter off */

/*  Register accessed at EN_CMD, the 8390 base addr.  */
#define E8390_STOP	0x01		/* Stop and reset the chip */
#define E8390_START	0x02		/* Start the chip, clear reset */
#define E8390_TRANS	0x04		/* Transmit a frame */
#define E8390_RREAD	0x08		/* Remote read */
#define E8390_RWRITE	0x10		/* Remote write  */
#define E8390_NODMA	0x20		/* Remote DMA */
#define E8390_PAGE0	0x00		/* Select page chip registers */
#define E8390_PAGE1	0x40		/* using the two high-order bits */
#define E8390_PAGE2	0x80		/* Page 3 is invalid. */

#define E8390_CMD	0x00		/* The command register (for all pages) */
/* Page 0 register offsets. */
#define EN0_CLDALO	0x01		/* Low byte of current local dma addr  RD */
#define EN0_STARTPG	0x01		/* Starting page of ring bfr WR */
#define EN0_CLDAHI	0x02		/* High byte of current local dma addr  RD */
#define EN0_STOPPG	0x02		/* Ending page +1 of ring bfr WR */
#define EN0_BOUNDARY	0x03		/* Boundary page of ring bfr RD WR */
#define EN0_TSR		0x04		/* Transmit status reg RD */
#define EN0_TPSR	0x04		/* Transmit starting page WR */
#define EN0_NCR		0x05		/* Number of collision reg RD */
#define EN0_TCNTLO	0x05		/* Low  byte of tx byte count WR */
#define EN0_FIFO	0x06		/* FIFO RD */
#define EN0_TCNTHI	0x06		/* High byte of tx byte count WR */
#define EN0_ISR		0x07		/* Interrupt status reg RD WR */
#define EN0_CRDALO	0x08		/* low byte of current remote dma address RD */
#define EN0_RSARLO	0x08		/* Remote start address reg 0 */
#define EN0_CRDAHI	0x09		/* high byte, current remote dma address RD */
#define EN0_RSARHI	0x09		/* Remote start address reg 1 */
#define EN0_RCNTLO	0x0a		/* Remote byte count reg WR */
#define EN0_RCNTHI	0x0b		/* Remote byte count reg WR */
#define EN0_RSR		0x0c		/* rx status reg RD */
#define EN0_RXCR	0x0c		/* RX configuration reg WR */
#define EN0_TXCR	0x0d		/* TX configuration reg WR */
#define EN0_COUNTER0	0x0d		/* Rcv alignment error counter RD */
#define EN0_DCFG	0x0e		/* Data configuration reg WR */
#define EN0_COUNTER1	0x0e		/* Rcv CRC error counter RD */
#define EN0_IMR		0x0f		/* Interrupt mask reg WR */
#define EN0_COUNTER2	0x0f		/* Rcv missed frame error counter RD */

/* Bits in EN0_ISR - Interrupt status register */
#define ENISR_RX	0x01		/* Receiver, no error */
#define ENISR_TX	0x02		/* Transmitter, no error */
#define ENISR_RX_ERR	0x04		/* Receiver, with error */
#define ENISR_TX_ERR	0x08		/* Transmitter, with error */
#define ENISR_OVER	0x10		/* Receiver overwrote the ring */
#define ENISR_COUNTERS	0x20		/* Counters need emptying */
#define ENISR_RDC	0x40		/* remote dma complete */
#define ENISR_RESET	0x80		/* Reset completed */
#define ENISR_ALL	0x3f		/* Interrupts we will enable */

/* Bits in EN0_DCFG - Data config register */
#define ENDCFG_WTS	0x01		/* word transfer mode selection */

/* Page 1 register offsets. */
#define EN1_PHYS0	0x01		/* This board's physical enet addr RD WR */
#define EN1_PHYS1	0x02		/* This board's physical enet addr RD WR */
#define EN1_PHYS2	0x03		/* This board's physical enet addr RD WR */
#define EN1_PHYS3	0x04		/* This board's physical enet addr RD WR */
#define EN1_PHYS4	0x05		/* This board's physical enet addr RD WR */
#define EN1_PHYS5	0x06		/* This board's physical enet addr RD WR */
#define EN1_CURPAG	0x07		/* Current memory page RD WR */
#define EN1_MULT0	0x08		/* Multicast filter mask array RD WR */
#define EN1_MULT1	0x09		/* Multicast filter mask array RD WR */
#define EN1_MULT2	0x0a		/* Multicast filter mask array RD WR */
#define EN1_MULT3	0x0b		/* Multicast filter mask array RD WR */
#define EN1_MULT4	0x0c		/* Multicast filter mask array RD WR */
#define EN1_MULT5	0x0d		/* Multicast filter mask array RD WR */
#define EN1_MULT6	0x0e		/* Multicast filter mask array RD WR */
#define EN1_MULT7	0x0f		/* Multicast filter mask array RD WR */

/* Bits in received packet status byte and EN0_RSR*/
#define ENRSR_RXOK	0x01		/* Received a good packet */
#define ENRSR_CRC	0x02		/* CRC error */
#define ENRSR_FAE	0x04		/* frame alignment error */
#define ENRSR_FO	0x08		/* FIFO overrun */
#define ENRSR_MPA	0x10		/* missed pkt */
#define ENRSR_PHY	0x20		/* physical/multicase address */
#define ENRSR_DIS	0x40		/* receiver disable. set in monitor mode */
#define ENRSR_DEF	0x80		/* deferring */

/* Transmitted packet status, EN0_TSR. */
#define ENTSR_PTX 0x01			/* Packet transmitted without error */
#define ENTSR_ND  0x02			/* The transmit wasn't deferred. */
#define ENTSR_COL 0x04			/* The transmit collided at least once. */
#define ENTSR_ABT 0x08  		/* The transmit collided 16 times, and was deferred. */
#define ENTSR_CRS 0x10			/* The carrier sense was lost. */
#define ENTSR_FU  0x20  		/* A "FIFO underrun" occurred during transmit. */
#define ENTSR_CDH 0x40			/* The collision detect "heartbeat" signal was lost. */
#define ENTSR_OWC 0x80  		/* There was an out-of-window collision. */


#define TX_2X_PAGES 12
#define TX_1X_PAGES 6

/* Should always use two Tx slots to get back-to-back transmits. */
#define EI_PINGPONG

#ifdef EI_PINGPONG
#define TX_PAGES TX_2X_PAGES
#else
#define TX_PAGES TX_1X_PAGES
#endif

#define ETHER_ADDR_LEN 6

/* The 8390 specific per-packet-header format. */
struct e8390_pkt_hdr {
  unsigned char status; /* status */
  unsigned char next;   /* pointer to next packet. */
  unsigned short count; /* header + packet length in bytes */
};

/* Most of these entries should be in 'struct device' (or most of the
   things in there should be here!) */
/* You have one of these per-board */
struct ei_device {
  const char *name;
  void (*reset_8390)(struct device *);
  void (*get_8390_hdr)(struct device *, struct e8390_pkt_hdr *, int);
  void (*block_output)(struct device *, int, const unsigned char *, int);
  void (*block_input)(struct device *, int, struct sk_buff *, int);
  unsigned open:1;
  unsigned word16:1;  /* We have the 16-bit (vs 8-bit) version of the card. */
  unsigned txing:1;		/* Transmit Active */
  unsigned irqlock:1;		/* 8390's intrs disabled when '1'. */
  unsigned dmaing:1;		/* Remote DMA Active */
  unsigned char tx_start_page, rx_start_page, stop_page;
  unsigned char current_page;	/* Read pointer in buffer  */
  unsigned char interface_num;	/* Net port (AUI, 10bT.) to use. */
  unsigned char txqueue;	/* Tx Packet buffer queue length. */
  short tx1, tx2;		/* Packet lengths for ping-pong tx. */
  short lasttx;			/* Alpha version consistency check. */
  unsigned char reg0;		/* Register '0' in a WD8013 */
  unsigned char reg5;		/* Register '5' in a WD8013 */
  unsigned char saved_irq;	/* Original dev->irq value. */
  /* The new statistics table. */
  struct enet_statistics stat;
};

/* The maximum number of 8390 interrupt service routines called per IRQ. */
#define MAX_SERVICE 12

/* The maximum time waited (in jiffies) before assuming a Tx failed. (20ms) */
#define TX_TIMEOUT (20*HZ/100)

#define ei_status (*(struct ei_device *)(dev->priv))


#endif
