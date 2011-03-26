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

#include <linux/module.h>
#include <linux/stddef.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/ptrace.h>
#include <linux/errno.h>
#include <linux/ioport.h>
#include <linux/malloc.h>
#include <linux/interrupt.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>

#include <asm/bitops.h>
#include <asm/io.h>
#include <asm/irq.h>

#include <asm/setup.h>
#include <asm/amigaints.h>
#include <asm/amigahw.h>
#include <asm/zorro.h>
#include <asm/amigatypes.h>
#include <asm/amigayle.h>
#include <asm/amipcmcia.h>
#include "apne.h"

int apne_probe(struct device *dev);
static int apne_probe1(struct device *dev, int ioaddr);

static int apne_open(struct device *dev);
static int apne_close(struct device *dev);

static void apne_reset_8390(struct device *dev);
static void apne_get_8390_hdr(struct device *dev, struct e8390_pkt_hdr *hdr,
			  int ring_page);
static void apne_block_input(struct device *dev, int count,
			  struct sk_buff *skb, int ring_offset);
static void apne_block_output(struct device *dev, const int count,
		const unsigned char *buf, const int start_page);


static int ethdev_init(struct device *dev);

#define ei_reset_8390 (ei_local->reset_8390)
#define ei_block_output (ei_local->block_output)
#define ei_block_input (ei_local->block_input)
#define ei_get_8390_hdr (ei_local->get_8390_hdr)

static int ei_open(struct device *dev);
static int ei_close(struct device *dev);
static void ei_interrupt(int irq, void *dev_id, struct pt_regs *regs);

static void ei_tx_intr(struct device *dev);
static void ei_tx_err(struct device *dev);
static void ei_receive(struct device *dev);
static void ei_rx_overrun(struct device *dev);

static void NS8390_trigger_send(struct device *dev, unsigned int length,
								int start_page);

static int init_pcmcia(void);

/* use 0 for production, 1 for verification, >2 for debug */
/*#define EI_DEBUG 4*/

#ifdef EI_DEBUG
int apne_debug = EI_DEBUG;
#else
int apne_debug = 1;
#endif

/* IO base address used for nic */

#define IOBASE 0x300

/*
   use MANUAL_CONFIG and MANUAL_OFFSET for enabling IO by hand
   you can find the values to use by looking at the cnetdevice
   config file example (the default values are for the CNET40BC card)
*/

/*
#define MANUAL_CONFIG 0x20
#define MANUAL_OFFSET 0x3f8

#define MANUAL_HWADDR0 0x00
#define MANUAL_HWADDR1 0x12
#define MANUAL_HWADDR2 0x34
#define MANUAL_HWADDR3 0x56
#define MANUAL_HWADDR4 0x78
#define MANUAL_HWADDR5 0x9a
*/

#define WORDSWAP(a) ( (((a)>>8)&0xff) | ((a)<<8) )

static const char *version =
    "apne.c:v1.0 11/29/97 Alain Malek (Alain.Malek@cryogen.com)\n";

/*-*************************************************************************-*/

int apne_probe(struct device *dev)
{
#ifndef MANUAL_CONFIG
	char tuple[8];
#endif

	if ( !(MACH_IS_AMIGA &&
	       (boot_info.bi_un.bi_ami.model == AMI_1200 ||
		boot_info.bi_un.bi_ami.model == AMI_600)) )
		return -ENODEV;

	printk("Looking for PCMCIA ethernet card : ");

	/* check if a card is inserted */
	if (!(PCMCIA_INSERTED)) {
		printk("NO PCMCIA card inserted\n");
		return -ENODEV;
	}

	/* disable pcmcia irq for readtuple */
	pcmcia_disable_irq();

#ifndef MANUAL_CONFIG
	if ((pcmcia_copy_tuple(CISTPL_FUNCID, tuple, 8) < 3) ||
	     (tuple[2] != CISTPL_FUNCID_NETWORK)) {
		printk("not an ethernet card\n");
		return -ENODEV;
	}
#endif

	printk("ethernet PCMCIA card inserted\n");

	if (init_pcmcia())
		return apne_probe1(dev, IOBASE);
	else
		return -ENODEV;

}

/*-*************************************************************************-*/
static int apne_open(struct device *dev)
{
    ei_open(dev);
    MOD_INC_USE_COUNT;
    return 0;
}

static int apne_close(struct device *dev)
{
    if (apne_debug > 1)
	printk("%s: Shutting down ethercard.\n", dev->name);
    ei_close(dev);
    MOD_DEC_USE_COUNT;
    return 0;
}
/*-*************************************************************************-*/

/* This page of functions should be 8390 generic */
/* Follow National Semi's recommendations for initializing the "NIC". */

static void apNS8390_init(struct device *dev, int startp)
{
    int e8390_base = dev->base_addr;
    struct ei_device *ei_local = (struct ei_device *) dev->priv;
    int i;
    int endcfg = ei_local->word16 ? (0x48 | ENDCFG_WTS) : 0x48;
    unsigned long flags;
    
    /* Follow National Semi's recommendations for initing the DP83902. */
    gayle_outb(E8390_NODMA+E8390_PAGE0+E8390_STOP, e8390_base); /* 0x21 */
    gayle_outb(endcfg, e8390_base + EN0_DCFG);	/* 0x48 or 0x49 */
    /* Clear the remote byte count registers. */
    gayle_outb(0x00,  e8390_base + EN0_RCNTLO);
    gayle_outb(0x00,  e8390_base + EN0_RCNTHI);
    /* Set to monitor and loopback mode -- this is vital!. */
    gayle_outb(E8390_RXOFF, e8390_base + EN0_RXCR); /* 0x20 */
    gayle_outb(E8390_TXOFF, e8390_base + EN0_TXCR); /* 0x02 */
    /* Set the transmit page and receive ring. */
    gayle_outb(ei_local->tx_start_page, e8390_base + EN0_TPSR);
    ei_local->tx1 = ei_local->tx2 = 0;
    gayle_outb(ei_local->rx_start_page, e8390_base + EN0_STARTPG);
    gayle_outb(ei_local->stop_page-1, e8390_base + EN0_BOUNDARY); /* 3c503 says 0x3f,NS0x26*/
    ei_local->current_page = ei_local->rx_start_page;		/* assert boundary+1 */
    gayle_outb(ei_local->stop_page, e8390_base + EN0_STOPPG);
    /* Clear the pending interrupts and mask. */
    gayle_outb(0xFF, e8390_base + EN0_ISR);
    gayle_outb(0x00,  e8390_base + EN0_IMR);
    
    /* Copy the station address into the DS8390 registers,
       and set the multicast hash bitmap to receive all multicasts. */
    save_flags(flags);
    cli();
    gayle_outb(E8390_NODMA + E8390_PAGE1 + E8390_STOP, e8390_base); /* 0x61 */

    {
	unsigned long en1_phys[6] = {EN1_PHYS0, EN1_PHYS1, EN1_PHYS2,
				     EN1_PHYS3, EN1_PHYS4, EN1_PHYS5 };
        for(i = 0; i < 6; i++) {
		    gayle_outb(dev->dev_addr[i], e8390_base + en1_phys[i]);
        }
    }

    /* Initialize the multicast list to accept-all.  If we enable multicast
       the higher levels can do the filtering. */
    {
	unsigned long en1_mult[8] = {EN1_MULT0, EN1_MULT1, EN1_MULT2, EN1_MULT3,
				     EN1_MULT4, EN1_MULT5, EN1_MULT6, EN1_MULT7 };
        for(i = 0; i < 8; i++) {
            gayle_outb(0xff, e8390_base + en1_mult[i]);
        }
    }

    gayle_outb(ei_local->rx_start_page, e8390_base + EN1_CURPAG);
    gayle_outb(E8390_NODMA+E8390_PAGE0+E8390_STOP, e8390_base);
    restore_flags(flags);
    dev->tbusy = 0;
    dev->interrupt = 0;
    ei_local->tx1 = ei_local->tx2 = 0;
    ei_local->txing = 0;
    if (startp) {
		gayle_outb(0xff, e8390_base + EN0_ISR);
		gayle_outb(ENISR_ALL, e8390_base + EN0_IMR);
		gayle_outb(E8390_NODMA+E8390_PAGE0+E8390_START, e8390_base);
		gayle_outb(E8390_TXCONFIG, e8390_base + EN0_TXCR); /* xmit on. */
		/* 3c503 TechMan says rxconfig only after the NIC is started. */
		gayle_outb(E8390_RXCONFIG, e8390_base + EN0_RXCR); /* rx on,  */
		dev->set_multicast_list(dev);		/* Get the multicast status right if this
							   was a reset. */
    }
    return;
}

/*-*************************************************************************-*/

static int apne_probe1(struct device *dev, int ioaddr)
{
    int i;
    unsigned char SA_prom[32];
    int wordlength = 2;
    const char *name = NULL;
    int start_page, stop_page;
#ifndef MANUAL_HWADDR0
    int neX000, ctron;
#endif
    static unsigned version_printed = 0;

    /* We should have a "dev" from Space.c or the static module table. */
    if (dev == NULL) {
	printk(KERN_ERR "apne.c: Passed a NULL device.\n");
	dev = init_etherdev(0, 0);
    }

    if (apne_debug  &&  version_printed++ == 0)
	printk(version);

    printk("PCMCIA NE*000 ethercard probe");

    /* Reset card. Who knows what dain-bramaged state it was left in. */
    {	unsigned long reset_start_time = jiffies;

	gayle_outb(gayle_inb(ioaddr + NE_RESET), ioaddr + NE_RESET);

	while ((gayle_inb(ioaddr + EN0_ISR) & ENISR_RESET) == 0)
		if (jiffies - reset_start_time > 2*HZ/100) {
			printk(" not found (no reset ack).\n");
			return ENODEV;
		}

	gayle_outb(0xff, ioaddr + EN0_ISR);		/* Ack all intr. */
    }

#ifndef MANUAL_HWADDR0

    /* Read the 16 bytes of station address PROM.
       We must first initialize registers, similar to apNS8390_init(eifdev, 0).
       We can't reliably read the SAPROM address without this.
       (I learned the hard way!). */
    {
	struct {unsigned long value, offset; } program_seq[] = {
	    {E8390_NODMA+E8390_PAGE0+E8390_STOP, E8390_CMD}, /* Select page 0*/
	    {0x48,	EN0_DCFG},	/* Set byte-wide (0x48) access. */
	    {0x00,	EN0_RCNTLO},	/* Clear the count regs. */
	    {0x00,	EN0_RCNTHI},
	    {0x00,	EN0_IMR},	/* Mask completion irq. */
	    {0xFF,	EN0_ISR},
	    {E8390_RXOFF, EN0_RXCR},	/* 0x20  Set to monitor */
	    {E8390_TXOFF, EN0_TXCR},	/* 0x02  and loopback mode. */
	    {32,	EN0_RCNTLO},
	    {0x00,	EN0_RCNTHI},
	    {0x00,	EN0_RSARLO},	/* DMA starting at 0x0000. */
	    {0x00,	EN0_RSARHI},
	    {E8390_RREAD+E8390_START, E8390_CMD},
	};
	for (i = 0; i < sizeof(program_seq)/sizeof(program_seq[0]); i++) {
	    gayle_outb(program_seq[i].value, ioaddr + program_seq[i].offset);
	}

    }
    for(i = 0; i < 32 /*sizeof(SA_prom)*/; i+=2) {
	SA_prom[i] = gayle_inb(ioaddr + NE_DATAPORT);
	SA_prom[i+1] = gayle_inb(ioaddr + NE_DATAPORT);
	if (SA_prom[i] != SA_prom[i+1])
	    wordlength = 1;
    }

    /*	At this point, wordlength *only* tells us if the SA_prom is doubled
	up or not because some broken PCI cards don't respect the byte-wide
	request in program_seq above, and hence don't have doubled up values. 
	These broken cards would otherwise be detected as an ne1000.  */

    if (wordlength == 2)
	for (i = 0; i < 16; i++)
		SA_prom[i] = SA_prom[i+i];
    
    if (wordlength == 2) {
	/* We must set the 8390 for word mode. */
	gayle_outb(0x49, ioaddr + EN0_DCFG);
	start_page = NESM_START_PG;
	stop_page = NESM_STOP_PG;
    } else {
	start_page = NE1SM_START_PG;
	stop_page = NE1SM_STOP_PG;
    }

    neX000 = (SA_prom[14] == 0x57  &&  SA_prom[15] == 0x57);
    ctron =  (SA_prom[0] == 0x00 && SA_prom[1] == 0x00 && SA_prom[2] == 0x1d);

    /* Set up the rest of the parameters. */
    if (neX000) {
	name = (wordlength == 2) ? "NE2000" : "NE1000";
    } else if (ctron) {
	name = (wordlength == 2) ? "Ctron-8" : "Ctron-16";
	start_page = 0x01;
	stop_page = (wordlength == 2) ? 0x40 : 0x20;
    } else {
	printk(" not found.\n");
	return ENXIO;

    }

#else
    wordlength = 2;
    /* We must set the 8390 for word mode. */
    gayle_outb(0x49, ioaddr + EN0_DCFG);
    start_page = NESM_START_PG;
    stop_page = NESM_STOP_PG;

    SA_prom[0] = MANUAL_HWADDR0;
    SA_prom[1] = MANUAL_HWADDR1;
    SA_prom[2] = MANUAL_HWADDR2;
    SA_prom[3] = MANUAL_HWADDR3;
    SA_prom[4] = MANUAL_HWADDR4;
    SA_prom[5] = MANUAL_HWADDR5;
    name = "NE2000";
#endif

    dev->base_addr = ioaddr;

    /* Install the Interrupt handler */
    if (request_irq(IRQ_AMIGA_PORTS, ei_interrupt, 0, "apne Ethernet", dev))
        return -EAGAIN;


    /* Allocate dev->priv and fill in 8390 specific dev fields. */
    if (ethdev_init(dev)) {
	printk (" unable to get memory for dev->priv.\n");
	return -ENOMEM;
    }

    for(i = 0; i < ETHER_ADDR_LEN; i++) {
	printk(" %2.2x", SA_prom[i]);
	dev->dev_addr[i] = SA_prom[i];
    }

    printk("\n%s: %s found.\n",
	   dev->name, name);

    ei_status.name = name;
    ei_status.tx_start_page = start_page;
    ei_status.stop_page = stop_page;
    ei_status.word16 = (wordlength == 2);

    ei_status.rx_start_page = start_page + TX_PAGES;

    ei_status.reset_8390 = &apne_reset_8390;
    ei_status.block_input = &apne_block_input;
    ei_status.block_output = &apne_block_output;
    ei_status.get_8390_hdr = &apne_get_8390_hdr;
    dev->open = &apne_open;
    dev->stop = &apne_close;
    apNS8390_init(dev, 0);

    pcmcia_ack_int(pcmcia_get_intreq());		/* ack PCMCIA int req */
    pcmcia_enable_irq();

    return 0;
}

/*-*************************************************************************-*/

/* Hard reset the card.  This used to pause for the same period that a
   8390 reset command required, but that shouldn't be necessary. */
static void
apne_reset_8390(struct device *dev)
{
    unsigned long reset_start_time = jiffies;

    init_pcmcia();

    if (apne_debug > 1) printk("resetting the 8390 t=%ld...", jiffies);

    gayle_outb(gayle_inb(NE_BASE + NE_RESET), NE_BASE + NE_RESET);

    ei_status.txing = 0;
    ei_status.dmaing = 0;

    /* This check _should_not_ be necessary, omit eventually. */
    while ((gayle_inb(NE_BASE+EN0_ISR) & ENISR_RESET) == 0)
	if (jiffies - reset_start_time > 2*HZ/100) {
	    printk("%s: ne_reset_8390() did not complete.\n", dev->name);
	    break;
	}
    gayle_outb(ENISR_RESET, NE_BASE + EN0_ISR);	/* Ack intr. */
}

/*-*************************************************************************-*/
/* Grab the 8390 specific header. Similar to the block_input routine, but
   we don't need to be concerned with ring wrap as the header will be at
   the start of a page, so we optimize accordingly. */

static void
apne_get_8390_hdr(struct device *dev, struct e8390_pkt_hdr *hdr, int ring_page)
{

    int nic_base = dev->base_addr;
    int cnt;
    char *ptrc;
    short *ptrs;

    /* This *shouldn't* happen. If it does, it's the last thing you'll see */
    if (ei_status.dmaing) {
	printk("%s: DMAing conflict in ne_get_8390_hdr "
	   "[DMAstat:%d][irqlock:%d][intr:%d].\n",
	   dev->name, ei_status.dmaing, ei_status.irqlock,
	   dev->interrupt);
	return;
    }

    ei_status.dmaing |= 0x01;
    gayle_outb(E8390_NODMA+E8390_PAGE0+E8390_START, nic_base+ NE_CMD);
    gayle_outb(ENISR_RDC, nic_base + EN0_ISR);
    gayle_outb(sizeof(struct e8390_pkt_hdr), nic_base + EN0_RCNTLO);
    gayle_outb(0, nic_base + EN0_RCNTHI);
    gayle_outb(0, nic_base + EN0_RSARLO);		/* On page boundary */
    gayle_outb(ring_page, nic_base + EN0_RSARHI);
    gayle_outb(E8390_RREAD+E8390_START, nic_base + NE_CMD);

    if (ei_status.word16) {
        ptrs = (short*)hdr;
        for(cnt = 0; cnt < (sizeof(struct e8390_pkt_hdr)>>1); cnt++)
            *ptrs++ = gayle_inw(NE_BASE + NE_DATAPORT);
    } else {
        ptrc = (char*)hdr;
        for(cnt = 0; cnt < sizeof(struct e8390_pkt_hdr); cnt++)
            *ptrc++ = gayle_inb(NE_BASE + NE_DATAPORT);
    }

    gayle_outb(ENISR_RDC, nic_base + EN0_ISR);	/* Ack intr. */

    hdr->count = WORDSWAP(hdr->count);

    ei_status.dmaing &= ~0x01;
}

/*-*************************************************************************-*/
/* Block input and output, similar to the Crynwr packet driver.  If you
   are porting to a new ethercard, look at the packet driver source for hints.
   The NEx000 doesn't share the on-board packet memory -- you have to put
   the packet out through the "remote DMA" dataport using outb. */

static void
apne_block_input(struct device *dev, int count, struct sk_buff *skb, int ring_offset)
{
    int nic_base = dev->base_addr;
    char *buf = skb->data;
    char *ptrc;
    short *ptrs;
    int cnt;

    /* This *shouldn't* happen. If it does, it's the last thing you'll see */
    if (ei_status.dmaing) {
	printk("%s: DMAing conflict in ne_block_input "
	   "[DMAstat:%d][irqlock:%d][intr:%d].\n",
	   dev->name, ei_status.dmaing, ei_status.irqlock,
	   dev->interrupt);
	return;
    }
    ei_status.dmaing |= 0x01;
    gayle_outb(E8390_NODMA+E8390_PAGE0+E8390_START, nic_base+ NE_CMD);
    gayle_outb(ENISR_RDC, nic_base + EN0_ISR);
    gayle_outb(count & 0xff, nic_base + EN0_RCNTLO);
    gayle_outb(count >> 8, nic_base + EN0_RCNTHI);
    gayle_outb(ring_offset & 0xff, nic_base + EN0_RSARLO);
    gayle_outb(ring_offset >> 8, nic_base + EN0_RSARHI);
    gayle_outb(E8390_RREAD+E8390_START, nic_base + NE_CMD);
    if (ei_status.word16) {
      ptrs = (short*)buf;
      for (cnt = 0; cnt < (count>>1); cnt++)
        *ptrs++ = gayle_inw(NE_BASE + NE_DATAPORT);
      if (count & 0x01) {
	buf[count-1] = gayle_inb(NE_BASE + NE_DATAPORT);
      }
    } else {
      ptrc = (char*)buf;
      for (cnt = 0; cnt < count; cnt++)
        *ptrc++ = gayle_inb(NE_BASE + NE_DATAPORT);
    }

    gayle_outb(ENISR_RDC, nic_base + EN0_ISR);	/* Ack intr. */
    ei_status.dmaing &= ~0x01;
}

/*-*************************************************************************-*/

static void
apne_block_output(struct device *dev, int count,
		const unsigned char *buf, const int start_page)
{
    int nic_base = NE_BASE;
    unsigned long dma_start;
    char *ptrc;
    short *ptrs;
    int cnt;

    /* Round the count up for word writes.  Do we need to do this?
       What effect will an odd byte count have on the 8390?
       I should check someday. */
    if (ei_status.word16 && (count & 0x01))
      count++;

    /* This *shouldn't* happen. If it does, it's the last thing you'll see */
    if (ei_status.dmaing) {
	printk("%s: DMAing conflict in ne_block_output."
	   "[DMAstat:%d][irqlock:%d][intr:%d]\n",
	   dev->name, ei_status.dmaing, ei_status.irqlock,
	   dev->interrupt);
	return;
    }
    ei_status.dmaing |= 0x01;
    /* We should already be in page 0, but to be safe... */
    gayle_outb(E8390_PAGE0+E8390_START+E8390_NODMA, nic_base + NE_CMD);

    gayle_outb(ENISR_RDC, nic_base + EN0_ISR);

   /* Now the normal output. */
    gayle_outb(count & 0xff, nic_base + EN0_RCNTLO);
    gayle_outb(count >> 8,   nic_base + EN0_RCNTHI);
    gayle_outb(0x00, nic_base + EN0_RSARLO);
    gayle_outb(start_page, nic_base + EN0_RSARHI);

    gayle_outb(E8390_RWRITE+E8390_START, nic_base + NE_CMD);
    if (ei_status.word16) {
        ptrs = (short*)buf;
        for (cnt = 0; cnt < count>>1; cnt++)
            gayle_outw(*ptrs++, NE_BASE+NE_DATAPORT);
    } else {
        ptrc = (char*)buf;
        for (cnt = 0; cnt < count; cnt++)
	    gayle_outb(*ptrc++, NE_BASE + NE_DATAPORT);
    }

    dma_start = jiffies;

    while ((gayle_inb(nic_base + EN0_ISR) & ENISR_RDC) == 0)
	if (jiffies - dma_start > 2*HZ/100) {		/* 20ms */
		printk("%s: timeout waiting for Tx RDC.\n", dev->name);
		apne_reset_8390(dev);
		apNS8390_init(dev,1);
		break;
	}

    gayle_outb(ENISR_RDC, nic_base + EN0_ISR);	/* Ack intr. */
    ei_status.dmaing &= ~0x01;
    return;
}

/*-*************************************************************************-*/
/* Open/initialize the board.  This routine goes all-out, setting everything
   up anew at each open, even though many of these registers should only
   need to be set once at boot.
   */
static int ei_open(struct device *dev)
{
    struct ei_device *ei_local = (struct ei_device *) dev->priv;

    /* This can't happen unless somebody forgot to call ethdev_init(). */
    if (ei_local == NULL) {
	printk(KERN_EMERG "%s: ei_open passed a non-existent device!\n", dev->name);
	return -ENXIO;
    }

    apNS8390_init(dev, 1);
    dev->start = 1;
    ei_local->irqlock = 0;
    return 0;
}

/* Opposite of above. Only used when "ifconfig <devname> down" is done. */
static int ei_close(struct device *dev)
{
    apNS8390_init(dev, 0);
    dev->start = 0;
    return 0;
}

/*-*************************************************************************-*/

static int ei_start_xmit(struct sk_buff *skb, struct device *dev)
{
    int e8390_base = dev->base_addr;
    struct ei_device *ei_local = (struct ei_device *) dev->priv;
    int length, send_length, output_page;

/*
 *  We normally shouldn't be called if dev->tbusy is set, but the
 *  existing code does anyway. If it has been too long since the
 *  last Tx, we assume the board has died and kick it.
 */
 
    if (dev->tbusy) {	/* Do timeouts, just like the 8003 driver. */
		int txsr = gayle_inb(e8390_base+EN0_TSR), isr;
		int tickssofar = jiffies - dev->trans_start;
		if (tickssofar < TX_TIMEOUT ||	(tickssofar < (TX_TIMEOUT+5) && ! (txsr & ENTSR_PTX))) {
			return 1;
		}
		isr = gayle_inb(e8390_base+EN0_ISR);
		if (dev->start == 0) {
			printk("%s: xmit on stopped card\n", dev->name);
			return 1;
		}

		/*
		 * Note that if the Tx posted a TX_ERR interrupt, then the
		 * error will have been handled from the interrupt handler.
		 * and not here.
		 */

		printk(KERN_DEBUG "%s: Tx timed out, %s TSR=%#2x, ISR=%#2x, t=%d.\n",
		   dev->name, (txsr & ENTSR_ABT) ? "excess collisions." :
		   (isr) ? "lost interrupt?" : "cable problem?", txsr, isr, tickssofar);

		if (!isr && !ei_local->stat.tx_packets) {
		   /* The 8390 probably hasn't gotten on the cable yet. */
		   ei_local->interface_num ^= 1;   /* Try a different xcvr.  */
		}

		/* Try to restart the card.  Perhaps the user has fixed something. */
		ei_reset_8390(dev);
		apNS8390_init(dev, 1);
		dev->trans_start = jiffies;
    }

    /* Sending a NULL skb means some higher layer thinks we've missed an
       tx-done interrupt. Caution: dev_tint() handles the cli()/sti()
       itself. */
    if (skb == NULL) {
		dev_tint(dev);
		return 0;
    }

    length = skb->len;
    if (skb->len <= 0)
		return 0;

    /* Mask interrupts from the ethercard. */
    gayle_outb(0x00, e8390_base + EN0_IMR);
    /* disable PCMCIA interrupt */
    pcmcia_disable_irq();
    if (dev->interrupt) {
	printk("%s: Tx request while isr active.\n",dev->name);
	gayle_outb(ENISR_ALL, e8390_base + EN0_IMR);
	return 1;
    }
    ei_local->irqlock = 1;

    send_length = ETH_ZLEN < length ? length : ETH_ZLEN;

#ifdef EI_PINGPONG

    /*
     * We have two Tx slots available for use. Find the first free
     * slot, and then perform some sanity checks. With two Tx bufs,
     * you get very close to transmitting back-to-back packets. With
     * only one Tx buf, the transmitter sits idle while you reload the
     * card, leaving a substantial gap between each transmitted packet.
     */

    if (ei_local->tx1 == 0) {
	output_page = ei_local->tx_start_page;
	ei_local->tx1 = send_length;
	if (apne_debug  &&  ei_local->tx2 > 0)
		printk("%s: idle transmitter tx2=%d, lasttx=%d, txing=%d.\n",
			dev->name, ei_local->tx2, ei_local->lasttx, ei_local->txing);
    } else if (ei_local->tx2 == 0) {
	output_page = ei_local->tx_start_page + TX_1X_PAGES;
	ei_local->tx2 = send_length;
	if (apne_debug  &&  ei_local->tx1 > 0)
		printk("%s: idle transmitter, tx1=%d, lasttx=%d, txing=%d.\n",
			dev->name, ei_local->tx1, ei_local->lasttx, ei_local->txing);
    } else {	/* We should never get here. */
	if (apne_debug)
		printk("%s: No Tx buffers free! irq=%d tx1=%d tx2=%d last=%d\n",
			dev->name, dev->interrupt, ei_local->tx1, ei_local->tx2, ei_local->lasttx);
	ei_local->irqlock = 0;
	dev->tbusy = 1;
	gayle_outb(ENISR_ALL, e8390_base + EN0_IMR);
        /* enable PCMCIA IRQ interrupt */
	pcmcia_enable_irq();
	return 1;
    }

    /*
     * Okay, now upload the packet and trigger a send if the transmitter
     * isn't already sending. If it is busy, the interrupt handler will
     * trigger the send later, upon receiving a Tx done interrupt.
     */

    ei_block_output(dev, length, skb->data, output_page);
    if (! ei_local->txing) {
	ei_local->txing = 1;
	NS8390_trigger_send(dev, send_length, output_page);
	dev->trans_start = jiffies;
	if (output_page == ei_local->tx_start_page) {
		ei_local->tx1 = -1;
		ei_local->lasttx = -1;
	} else {
		ei_local->tx2 = -1;
		ei_local->lasttx = -2;
	}
    } else
	ei_local->txqueue++;

    dev->tbusy = (ei_local->tx1  &&  ei_local->tx2);

#else	/* EI_PINGPONG */

    /*
     * Only one Tx buffer in use. You need two Tx bufs to come close to
     * back-to-back transmits. Expect a 20 -> 25% performance hit on
     * reasonable hardware if you only use one Tx buffer.
     */

    ei_block_output(dev, length, skb->data, ei_local->tx_start_page);
    ei_local->txing = 1;
    NS8390_trigger_send(dev, send_length, ei_local->tx_start_page);
    dev->trans_start = jiffies;
    dev->tbusy = 1;

#endif	/* EI_PINGPONG */

    /* Turn 8390 interrupts back on. */
    ei_local->irqlock = 0;
    gayle_outb(ENISR_ALL, e8390_base + EN0_IMR);
    /* enable PCMCIA IRQ interrupt */
    pcmcia_enable_irq();

    dev_kfree_skb (skb, FREE_WRITE);
    
    return 0;
}

/*-*************************************************************************-*/

static struct enet_statistics *get_stats(struct device *dev)
{
    int ioaddr = dev->base_addr;
    struct ei_device *ei_local = (struct ei_device *) dev->priv;
    
    /* If the card is stopped, just return the present stats. */
    if (dev->start == 0) return &ei_local->stat;

    /* Read the counter registers, assuming we are in page 0. */
    ei_local->stat.rx_frame_errors += gayle_inb(ioaddr + EN0_COUNTER0);
    ei_local->stat.rx_crc_errors   += gayle_inb(ioaddr + EN0_COUNTER1);
    ei_local->stat.rx_missed_errors+= gayle_inb(ioaddr + EN0_COUNTER2);

    return &ei_local->stat;
}

/*-*************************************************************************-*/
/*
 * A transmitter error has happened. Most likely excess collisions (which
 * is a fairly normal condition). If the error is one where the Tx will
 * have been aborted, we try and send another one right away, instead of
 * letting the failed packet sit and collect dust in the Tx buffer. This
 * is a much better solution as it avoids kernel based Tx timeouts, and
 * an unnecessary card reset.
 */

static void ei_tx_err(struct device *dev)
{
    int e8390_base = dev->base_addr;
    unsigned char txsr = gayle_inb(e8390_base+EN0_TSR);
    unsigned char tx_was_aborted = txsr & (ENTSR_ABT+ENTSR_FU);
    struct ei_device *ei_local = (struct ei_device *) dev->priv;

#ifdef VERBOSE_ERROR_DUMP
    printk(KERN_DEBUG "%s: transmitter error (%#2x): ", dev->name, txsr);
    if (txsr & ENTSR_ABT)
		printk("excess-collisions ");
    if (txsr & ENTSR_ND)
		printk("non-deferral ");
    if (txsr & ENTSR_CRS)
		printk("lost-carrier ");
    if (txsr & ENTSR_FU)
		printk("FIFO-underrun ");
    if (txsr & ENTSR_CDH)
		printk("lost-heartbeat ");
    printk("\n");
#endif

    gayle_outb(ENISR_TX_ERR, e8390_base + EN0_ISR); /* Ack intr. */

    if (tx_was_aborted)
		ei_tx_intr(dev);

    /*
     * Note: NCR reads zero on 16 collisions so we add them
     * in by hand. Somebody might care...
     */
    if (txsr & ENTSR_ABT)
	ei_local->stat.collisions += 16;
	
}

/*-*************************************************************************-*/
/* We have finished a transmit: check for errors and then trigger the next
   packet to be sent. */
static void ei_tx_intr(struct device *dev)
{
    int e8390_base = dev->base_addr;
    int status = gayle_inb(e8390_base + EN0_TSR);
    struct ei_device *ei_local = (struct ei_device *) dev->priv;
    
    gayle_outb(ENISR_TX, e8390_base + EN0_ISR); /* Ack intr. */

#ifdef EI_PINGPONG

    /*
     * There are two Tx buffers, see which one finished, and trigger
     * the send of another one if it exists.
     */
    ei_local->txqueue--;
    if (ei_local->tx1 < 0) {
	if (ei_local->lasttx != 1 && ei_local->lasttx != -1)
		printk("%s: bogus last_tx_buffer %d, tx1=%d.\n",
			   ei_local->name, ei_local->lasttx, ei_local->tx1);
	ei_local->tx1 = 0;
	dev->tbusy = 0;
	if (ei_local->tx2 > 0) {
		ei_local->txing = 1;
		NS8390_trigger_send(dev, ei_local->tx2, ei_local->tx_start_page + 6);
		dev->trans_start = jiffies;
		ei_local->tx2 = -1,
		ei_local->lasttx = 2;
	} else
		ei_local->lasttx = 20, ei_local->txing = 0;
    } else if (ei_local->tx2 < 0) {
	if (ei_local->lasttx != 2  &&  ei_local->lasttx != -2)
		printk("%s: bogus last_tx_buffer %d, tx2=%d.\n",
			   ei_local->name, ei_local->lasttx, ei_local->tx2);
	ei_local->tx2 = 0;
	dev->tbusy = 0;
	if (ei_local->tx1 > 0) {
		ei_local->txing = 1;
		NS8390_trigger_send(dev, ei_local->tx1, ei_local->tx_start_page);
		dev->trans_start = jiffies;
		ei_local->tx1 = -1;
		ei_local->lasttx = 1;
	} else
		ei_local->lasttx = 10, ei_local->txing = 0;
    } else
	printk("%s: unexpected TX-done interrupt, lasttx=%d.\n",
		   dev->name, ei_local->lasttx);

#else	/* EI_PINGPONG */
    /*
     *  Single Tx buffer: mark it free so another packet can be loaded.
     */
    ei_local->txing = 0;
    dev->tbusy = 0;
#endif

    /* Minimize Tx latency: update the statistics after we restart TXing. */
    if (status & ENTSR_COL)
	ei_local->stat.collisions++;
    if (status & ENTSR_PTX)
	ei_local->stat.tx_packets++;
    else {
	ei_local->stat.tx_errors++;
	if (status & ENTSR_ABT) ei_local->stat.tx_aborted_errors++;
	if (status & ENTSR_CRS) ei_local->stat.tx_carrier_errors++;
	if (status & ENTSR_FU)  ei_local->stat.tx_fifo_errors++;
	if (status & ENTSR_CDH) ei_local->stat.tx_heartbeat_errors++;
	if (status & ENTSR_OWC) ei_local->stat.tx_window_errors++;
    }

    mark_bh (NET_BH);
}

/*-*************************************************************************-*/
/* We have a good packet(s), get it/them out of the buffers. */

static void ei_receive(struct device *dev)
{
    int e8390_base = dev->base_addr;
    struct ei_device *ei_local = (struct ei_device *) dev->priv;
    unsigned char rxing_page, this_frame, next_frame;
    unsigned short current_offset;
    int rx_pkt_count = 0;
    struct e8390_pkt_hdr rx_frame;
    int num_rx_pages = ei_local->stop_page-ei_local->rx_start_page;

    while (++rx_pkt_count < 10) {
		int pkt_len;

		/* Get the rx page (incoming packet pointer). */
		gayle_outb(E8390_NODMA+E8390_PAGE1, e8390_base + E8390_CMD);
		rxing_page = gayle_inb(e8390_base + EN1_CURPAG);
		gayle_outb(E8390_NODMA+E8390_PAGE0, e8390_base + E8390_CMD);
		
		/* Remove one frame from the ring.  Boundary is always a page behind. */
		this_frame = gayle_inb(e8390_base + EN0_BOUNDARY) + 1;
		if (this_frame >= ei_local->stop_page)
			this_frame = ei_local->rx_start_page;
		
		/* Someday we'll omit the previous, iff we never get this message.
		   (There is at least one clone claimed to have a problem.)  */
		if (apne_debug > 0  &&  this_frame != ei_local->current_page)
			printk("%s: mismatched read page pointers %2x vs %2x.\n",
				   dev->name, this_frame, ei_local->current_page);
		
		if (this_frame == rxing_page)	/* Read all the frames? */
			break;				/* Done for now */
		
		current_offset = this_frame << 8;
		ei_get_8390_hdr(dev, &rx_frame, this_frame);
		
		pkt_len = rx_frame.count - sizeof(struct e8390_pkt_hdr);
		
		next_frame = this_frame + 1 + ((pkt_len+4)>>8);
		
		/* Check for bogosity warned by 3c503 book: the status byte is never
		   written.  This happened a lot during testing! This code should be
		   cleaned up someday. */
		if (rx_frame.next != next_frame
			&& rx_frame.next != next_frame + 1
			&& rx_frame.next != next_frame - num_rx_pages
			&& rx_frame.next != next_frame + 1 - num_rx_pages) {
			ei_local->current_page = rxing_page;
			gayle_outb(ei_local->current_page-1, e8390_base+EN0_BOUNDARY);
			ei_local->stat.rx_errors++;
			continue;
		}

		if (pkt_len < 60  ||  pkt_len > 1518) {
			if (apne_debug)
				printk("%s: bogus packet size: %d, status=%#2x nxpg=%#2x.\n",
					   dev->name, rx_frame.count, rx_frame.status,
					   rx_frame.next);
			ei_local->stat.rx_errors++;
		} else if ((rx_frame.status & 0x0F) == ENRSR_RXOK) {
			struct sk_buff *skb;
			
			skb = dev_alloc_skb(pkt_len+2);
			if (skb == NULL) {
				if (apne_debug > 1)
					printk("%s: Couldn't allocate a sk_buff of size %d.\n",
						   dev->name, pkt_len);
				ei_local->stat.rx_dropped++;
				break;
			} else {
				skb_reserve(skb,2);	/* IP headers on 16 byte boundaries */
				skb->dev = dev;
				skb_put(skb, pkt_len);	/* Make room */
				ei_block_input(dev, pkt_len, skb, current_offset + sizeof(rx_frame));
				skb->protocol=eth_type_trans(skb,dev);
				netif_rx(skb);
				ei_local->stat.rx_packets++;
			}
		} else {
			int errs = rx_frame.status;
			if (apne_debug)
				printk("%s: bogus packet: status=%#2x nxpg=%#2x size=%d\n",
					   dev->name, rx_frame.status, rx_frame.next,
					   rx_frame.count);
			if (errs & ENRSR_FO)
				ei_local->stat.rx_fifo_errors++;
		}
		next_frame = rx_frame.next;
		
		/* This _should_ never happen: it's here for avoiding bad clones. */
		if (next_frame >= ei_local->stop_page) {
			printk("%s: next frame inconsistency, %#2x\n", dev->name,
				   next_frame);
			next_frame = ei_local->rx_start_page;
		}
		ei_local->current_page = next_frame;
		gayle_outb(next_frame-1, e8390_base+EN0_BOUNDARY);
    }

    /* We used to also ack ENISR_OVER here, but that would sometimes mask
    a real overrun, leaving the 8390 in a stopped state with rec'vr off. */
    gayle_outb(ENISR_RX+ENISR_RX_ERR, e8390_base+EN0_ISR);
    return;
}

/*-*************************************************************************-*/
/* 
 * We have a receiver overrun: we have to kick the 8390 to get it started
 * again. Problem is that you have to kick it exactly as NS prescribes in
 * the updated datasheets, or "the NIC may act in an unpredictable manner."
 * This includes causing "the NIC to defer indefinitely when it is stopped
 * on a busy network."  Ugh.
 */
static void ei_rx_overrun(struct device *dev)
{
    int e8390_base = dev->base_addr;
    unsigned long wait_start_time;
    unsigned char was_txing, must_resend = 0;
    struct ei_device *ei_local = (struct ei_device *) dev->priv;
    
    /*
     * Record whether a Tx was in progress and then issue the
     * stop command.
     */
    was_txing = gayle_inb(e8390_base+E8390_CMD) & E8390_TRANS;
    gayle_outb(E8390_NODMA+E8390_PAGE0+E8390_STOP, e8390_base+E8390_CMD);
    
    if (apne_debug > 1)
	printk("%s: Receiver overrun.\n", dev->name);
    ei_local->stat.rx_over_errors++;
    
    /* 
     * Wait a full Tx time (1.2ms) + some guard time, NS says 1.6ms total.
     * Early datasheets said to poll the reset bit, but now they say that
     * it "is not a reliable indicator and subsequently should be ignored."
     * We wait at least 10ms.
     */
    wait_start_time = jiffies;
    while (jiffies - wait_start_time <= 1*HZ/100)
	barrier();

    /*
     * Reset RBCR[01] back to zero as per magic incantation.
     */
    gayle_outb(0x00, e8390_base+EN0_RCNTLO);
    gayle_outb(0x00, e8390_base+EN0_RCNTHI);

    /*
     * See if any Tx was interrupted or not. According to NS, this
     * step is vital, and skipping it will cause no end of havoc.
     */
    if (was_txing) { 
	unsigned char tx_completed = gayle_inb(e8390_base+EN0_ISR) & (ENISR_TX+ENISR_TX_ERR);
	if (!tx_completed) must_resend = 1;
    }

    /*
     * Have to enter loopback mode and then restart the NIC before
     * you are allowed to slurp packets up off the ring.
     */
    gayle_outb(E8390_TXOFF, e8390_base + EN0_TXCR);
    gayle_outb(E8390_NODMA + E8390_PAGE0 + E8390_START, e8390_base + E8390_CMD);

    /*
     * Clear the Rx ring of all the debris, and ack the interrupt.
     */
    ei_receive(dev);
    gayle_outb(ENISR_OVER, e8390_base+EN0_ISR);

    /*
     * Leave loopback mode, and resend any packet that got stopped.
     */
    gayle_outb(E8390_TXCONFIG, e8390_base + EN0_TXCR); 
    if (must_resend)
    	gayle_outb(E8390_NODMA + E8390_PAGE0 + E8390_START + E8390_TRANS, e8390_base + E8390_CMD);
	
}

/*-*************************************************************************-*/
/*
 *	Set or clear the multicast filter for this adaptor.
 */
 
static void set_multicast_list(struct device *dev)
{
	int ioaddr = dev->base_addr;
    
	if(dev->flags&IFF_PROMISC)
	{
		gayle_outb(E8390_RXCONFIG | 0x18, ioaddr + EN0_RXCR);
	}
	else if((dev->flags&IFF_ALLMULTI)||dev->mc_list)
	{
		/* The multicast-accept list is initialized to accept-all, and we
		   rely on higher-level filtering for now. */
		gayle_outb(E8390_RXCONFIG | 0x08, ioaddr + EN0_RXCR);
	} 
	else
		gayle_outb(E8390_RXCONFIG, ioaddr + EN0_RXCR);
}

/*-*************************************************************************-*/

/* Initialize the rest of the 8390 device structure. */
static int ethdev_init(struct device *dev)
{
    if (apne_debug > 1)
		printk(version);
    
    if (dev->priv == NULL) {
		struct ei_device *ei_local;
		
		dev->priv = kmalloc(sizeof(struct ei_device), GFP_KERNEL);
		if (dev->priv == NULL)
			return -ENOMEM;
		memset(dev->priv, 0, sizeof(struct ei_device));
		ei_local = (struct ei_device *)dev->priv;
    }
    
    dev->hard_start_xmit = &ei_start_xmit;
    dev->get_stats	= get_stats;
    dev->set_multicast_list = &set_multicast_list;

    ether_setup(dev);
        
    return 0;
}

/*-*************************************************************************-*/

/* Trigger a transmit start, assuming the length is valid. */
static void NS8390_trigger_send(struct device *dev, unsigned int length,
								int start_page)
{
    int e8390_base = dev->base_addr;
    
    gayle_outb(E8390_NODMA+E8390_PAGE0, e8390_base);
    
    if (gayle_inb(e8390_base) & E8390_TRANS) {
		printk("%s: trigger_send() called with the transmitter busy.\n",
			   dev->name);
		return;
    }
    gayle_outb(length & 0xff, e8390_base + EN0_TCNTLO);
    gayle_outb(length >> 8, e8390_base + EN0_TCNTHI);
    gayle_outb(start_page, e8390_base + EN0_TPSR);
    gayle_outb(E8390_NODMA+E8390_TRANS+E8390_START, e8390_base);
    return;
}

/*-*************************************************************************-*/
/* The typical workload of the driver:
   Handle the ether interface interrupts. */

static void ei_interrupt(int irq, void *dev_id, struct pt_regs * regs)
{
    struct device *dev;
    int e8390_base;
    int interrupts, nr_serviced = 0;
    struct ei_device *ei_local;
    unsigned char pcmcia_intreq;

    dev = (struct device *) dev_id;

    if (dev == NULL) {
		printk ("net_interrupt(): irq %d for unknown device.\n", irq);
		return;
    }

    if (!(gayle.inten & GAYLE_IRQ_IRQ))
	return;

    pcmcia_intreq = pcmcia_get_intreq();

    if (!(pcmcia_intreq & GAYLE_IRQ_IRQ)) {
        pcmcia_ack_int(pcmcia_intreq);
    	return;
    }
    if (apne_debug > 3)
        printk("pcmcia intreq = %x\n", pcmcia_intreq);

    e8390_base = dev->base_addr;
    ei_local = (struct ei_device *) dev->priv;
    if (dev->interrupt || ei_local->irqlock) {
		/* The "irqlock" check is only for testing. */
		printk(ei_local->irqlock
			   ? "%s: Interrupted while interrupts are masked! isr=%#2x imr=%#2x.\n"
			   : "%s: Reentering the interrupt handler! isr=%#2x imr=%#2x.\n",
			   dev->name, gayle_inb(e8390_base + EN0_ISR),
			   gayle_inb(e8390_base + EN0_IMR));
		return;
    }

    dev->interrupt = 1;

    /* Change to page 0 and read the intr status reg. */
    gayle_outb(E8390_NODMA+E8390_PAGE0, e8390_base + E8390_CMD);
    if (apne_debug > 3)
		printk("%s: interrupt(isr=%#2.2x).\n", dev->name,
			   gayle_inb(e8390_base + EN0_ISR));

    /* !!Assumption!! -- we stay in page 0.	 Don't break this. */
    while ((interrupts = gayle_inb(e8390_base + EN0_ISR)) != 0
		   && ++nr_serviced < MAX_SERVICE) {
		if (dev->start == 0) {
			printk("%s: interrupt from stopped card\n", dev->name);
			/*interrupts = 0;*/
			break;
		}
		if (interrupts & ENISR_OVER) {
			ei_rx_overrun(dev);
		} else if (interrupts & (ENISR_RX+ENISR_RX_ERR)) {
			/* Got a good (?) packet. */
			ei_receive(dev);
		}
		/* Push the next to-transmit packet through. */
		if (interrupts & ENISR_TX) {
			ei_tx_intr(dev);
		} else if (interrupts & ENISR_TX_ERR) {
			ei_tx_err(dev);
		}

		if (interrupts & ENISR_COUNTERS) {
			ei_local->stat.rx_frame_errors += gayle_inb(e8390_base + EN0_COUNTER0);
			ei_local->stat.rx_crc_errors   += gayle_inb(e8390_base + EN0_COUNTER1);
			ei_local->stat.rx_missed_errors+= gayle_inb(e8390_base + EN0_COUNTER2);
			gayle_outb(ENISR_COUNTERS, e8390_base + EN0_ISR); /* Ack intr. */
		}
		
		/* Ignore any RDC interrupts that make it back to here. */
		if (interrupts & ENISR_RDC) {
			gayle_outb(ENISR_RDC, e8390_base + EN0_ISR);
		}

		gayle_outb(E8390_NODMA+E8390_PAGE0+E8390_START, e8390_base + E8390_CMD);

		pcmcia_ack_int(pcmcia_get_intreq());
    }
    
    if (interrupts && apne_debug) {
		gayle_outb(E8390_NODMA+E8390_PAGE0+E8390_START, e8390_base + E8390_CMD);
		if (nr_serviced >= MAX_SERVICE) {
			printk("%s: Too much work at interrupt, status %#2.2x\n",
				   dev->name, interrupts);
			gayle_outb(ENISR_ALL, e8390_base + EN0_ISR); /* Ack. most intrs. */
		} else {
			printk("%s: unknown interrupt %#2x\n", dev->name, interrupts);
			gayle_outb(0xff, e8390_base + EN0_ISR); /* Ack. all intrs. */
		}
		pcmcia_ack_int(pcmcia_get_intreq());
    }
    dev->interrupt = 0;
    return;
}

/*-*************************************************************************-*/

#ifdef MODULE
static char devicename[9] = {0, };

static struct device apne_dev =
{
	devicename,
	0, 0, 0, 0,
	0, 0,
	0, 0, 0, NULL, apne_probe,
};

int init_module(void)
{
	int err;
	if ((err = register_netdev(&apne_dev))) {
		if (err == -EIO)
			printk("No PCMCIA NEx000 ethernet card found.\n");
		return (err);
	}
	return (0);
}

void cleanup_module(void)
{
	unregister_netdev(&apne_dev);

	pcmcia_disable_irq();

	free_irq(IRQ_AMIGA_PORTS, &apne_dev);

	pcmcia_reset();

}

#endif

/*-*************************************************************************-*/

static int init_pcmcia(void)
{
	u_char config;
#ifndef MANUAL_CONFIG
	u_char tuple[32];
	int offset_len;
#endif
	u_long offset;

	pcmcia_reset();
	pcmcia_program_voltage(PCMCIA_0V);
	pcmcia_access_speed(PCMCIA_SPEED_250NS);
	pcmcia_write_enable();

#ifdef MANUAL_CONFIG
	config = MANUAL_CONFIG;
#else
	/* get and write config byte to enable IO port */

	if (pcmcia_copy_tuple(CISTPL_CFTABLE_ENTRY, tuple, 32) < 3)
		return 0;

	config = tuple[2] & 0x3f;
#endif
#ifdef MANUAL_OFFSET
	offset = MANUAL_OFFSET;
#else
	if (pcmcia_copy_tuple(CISTPL_CONFIG, tuple, 32) < 6)
		return 0;

	offset_len = (tuple[2] & 0x3) + 1;
	offset = 0;
	while(offset_len--) {
		offset = (offset << 8) | tuple[4+offset_len];
	}
#endif

	outb(config, GAYLE_ATTRIBUTE+offset);

	return 1;
}

/*-*************************************************************************-*/
