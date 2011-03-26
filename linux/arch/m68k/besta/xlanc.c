/*
 * besta/xlanc.c -- Low level network driver for HCPU30 board.
 *
 * Copyright 1996, 1997	    Dmitry K. Butskoy
 *			    <buc@citadel.stu.neva.ru>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2, or (at your option) any
 * later version.
 *
 */

#include <linux/config.h>
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

#include "besta.h"
#include "hcpu30.h"

struct xlan {
	unsigned char x0;
	unsigned char x1;
	unsigned short x2;
	void *x4;
	unsigned short x8;
	unsigned short xa;
	void *xc;
	unsigned short x10;
	char r12;
	unsigned char x13;
	unsigned char x14;
	unsigned char x15;
	unsigned char x16[6];
	long r1c;
};

#define RX_NUM_BUF      32
#define TX_NUM_BUF      8

static int xlan_open (struct device *dev);
static int xlan_start_xmit (struct sk_buff *skb, struct device *dev);
static void xlan_intr (int vec, void *data, struct pt_regs *fp);
static int xlan_close (struct device *dev);
static struct enet_statistics *xlan_get_stats (struct device *dev);
static int do_xlan_init (struct device *);

#define XLANC_DMA_TX

#ifdef XLANC_DMA_RX
static struct lan_indata {
	unsigned short res;
	unsigned short len;
	unsigned char dest_addr[6];
	unsigned char src_addr[6];
	unsigned short sap;
	char data[0];
};

static struct lan_indata *lan_inbuf[RX_NUM_BUF];
static struct sk_buff *lan_inskb[RX_NUM_BUF];

#else  /*  ! XLANC_DMA_RX   */

static struct lan_indata {
	unsigned short res;
	unsigned short len;
	unsigned char dest_addr[6];
	unsigned char src_addr[6];
	unsigned short sap;
	char data[1570];
} lan_indata[RX_NUM_BUF + 41];
		/*  Why `+41'? On error DMA can full up to 1<<16 mem
		   by garbage. So, allocate space for this garbage. H-hhh... */

static struct lan_indata *lan_inbuf[RX_NUM_BUF];

#endif  /*  XLANC_DMA_RX   */


#ifdef XLANC_DMA_TX

static struct lan_outring {
	void            *addr;
	unsigned int    len;
	struct sk_buff  *skb;
} lan_outring[TX_NUM_BUF] = {{ NULL, 0, NULL }, };

#else   /* ! XLANC_DMA_TX   */

static struct lan_outbuf {
	unsigned char dest_addr[6];
	short sap;
	char buf[1572];
} lan_outbuf[TX_NUM_BUF];
static unsigned int lan_outlen[TX_NUM_BUF] = { 0, };
#endif  /*  XLANC_DMA_TX   */

static volatile int start_tx = 0, end_tx = 0, num_tx = 0;

static struct enet_statistics xlan_stats = { 0, };
static int xlconn = 0;


static struct device xlan_dev = {
    "eth0", 0, 0, 0, 0, 0, 0, 0, 0, 0, NULL, do_xlan_init, 0,
};


int xlan_init (void) {
	volatile struct xlan *v = (struct xlan *) XLAN_ADDR;
	int i;
	unsigned char c;
	int vector, level;

#ifdef XLANC_DMA_RX
	for (i=0;i < RX_NUM_BUF;i++) {

	    lan_inskb[i] = dev_alloc_skb (1584 + 2);
	    if (lan_inskb[i] == NULL) {
		int j;

		printk ("xlan: Cannot alloc Rx buffer \n");
		for (j = 0; j < i; j++)  kfree_skb (lan_inskb[j], 0);

		return -ENOMEM;
	    }

	    skb_reserve (skb, 2);       /*  16 byte align  */

	    lan_inbuf[i] = (struct lan_indata *) &lan_inskb[i]->data[-4];
					/*  there are 16 + 2 bytes reserved  */
	}
#else  /*  ! XLANC_DMA_RX   */
	for (i=0;i < RX_NUM_BUF;i++)  lan_inbuf[i] = &lan_indata[i];
#endif  /*  XLANC_DMA_RX   */

	if (!besta_get_vect_lev ("xlan", &vector, &level)) {
		vector = get_unused_vector();
		level = XLAN_LEV;
	}

	v->x2 = RX_NUM_BUF;
	v->x4 = lan_inbuf;
	v->x15 = vector;
	v->x14 = level;

	/*  we should overwrite first byte of eth-address
	   if it matches as multicast addr...
	*/
	if (v->x16[0] & 0x1)  v->x16[0] = 0;

	v->x0 = 1;
	while ((signed char) (c = v->x0) >= 0) ;

	v->x0 = 0;

	if (c != 128) {
		printk ("NO XLANC ");
		if (c != 195)  printk ("(xlanc init error %x) \n",c);
		else printk ("\n");

#ifdef XLANC_DMA_RX
		for (i=0;i < RX_NUM_BUF;i++)  kfree_skb (lan_inskb[i], 0);
#endif
		return 0;
	}

	printk ("xlan: ethernet address %02x:%02x:%02x:%02x:%02x:%02x \n",
		v->x16[0],v->x16[1],v->x16[2],v->x16[3],v->x16[4],v->x16[5]);

	/*  Add `xlan_dev' to the end of `dev_base' chain.
	    Then it will be initialized by net_dev_init() .    */

	if (dev_base == NULL)  dev_base = &xlan_dev;
	else {
	    struct device *dev = dev_base;

	    while (dev->next)  dev = dev->next;
	    dev->next = &xlan_dev;
	}
	xlan_dev.next = NULL;

	return 1;
}

static int do_xlan_init (struct device *dev) {
	volatile struct xlan *v = (struct xlan *) XLAN_ADDR;
	int i;
	static int init = 0;

	if (init)  return -ENODEV;      /*  no more devices on this...  */
	else  init = 1;

	/*  Note:  controller was previously initialized by xlan_init().  */

	for (i=0;i < 6;i++)  dev->dev_addr[i] = v->x16[i];

	init_etherdev (dev, 0);

	dev->open = &xlan_open;
	dev->stop = &xlan_close;
	dev->hard_start_xmit = &xlan_start_xmit;
	dev->get_stats = &xlan_get_stats;

	besta_handlers[v->x15] = xlan_intr;
	besta_intr_data[v->x15] = dev;

	printk ("xlan netdevice registered (%s)\n", dev->name);

	return 0;
}

static int xlan_open (struct device *dev) {

	dev->tbusy = 0;
	dev->interrupt = 0;
	dev->start = 1;

	return 0;
}

static int xlan_close (struct device *dev) {

	dev->start = 0;
	dev->tbusy = 1;

	return 0;
}


static void xlan_intr (int vec, void *data, struct pt_regs *fp) {
	volatile struct xlan *v = (struct xlan *) XLAN_ADDR;
	struct device *dev = (struct device *) data;
	unsigned short i;

	if (dev == NULL) {
		printk ("xlan_intr: unknown device\n");
		return;
	}
	if (dev->interrupt)
		printk ("xlan_intr: re-entering the interrupt handler\n");

	dev->interrupt = 1;

	if (v->x1 & 0x7f)  v->x1 = 0;

	while ((i = v->xa) != v->x8) {  /*  Rx interrupt   */
	    struct sk_buff *skb;
	    struct lan_indata *ldp = lan_inbuf[i];

	    /*  because fulled by DMA   */
	    clear_data_cache (ldp, sizeof (struct lan_indata));

	    /*  This is not needed by `030, because previous call
	      clear all the data cache, but be careful on `040 or `060
	       Actually for XLANC_DMA only.
	    clear_data_cache (ldp, ldp->len);
	    */

	    if (ldp->len > 1584) {
		printk ("xlan_intr: len error: len = 0x%x, res = 0x%x\n",
							ldp->len, ldp->res);
		xlan_stats.rx_errors++;
		goto tail;
	    }
	    if ((ldp->res & 0x20) == 0) {
	    /*  printk ("xlan_intr: Read Error: res = 0x%x, len = 0x%x\n",
							ldp->res, ldp->len);*/
		xlan_stats.rx_errors++;
		goto tail;
	    }
	    if (ldp->len < ETH_ZLEN) {  /*  ignore packets less then 60  */
		xlan_stats.rx_errors++;
		goto tail;
	    }

#ifdef XLANC_DMA_RX
	    skb = lan_inskb[i];
	    skb->dev = dev;
	    skb_put (skb, ldp->len);
	    skb->ip_summed = CHECKSUM_UNNECESSARY;
	    skb->protocol = eth_type_trans (skb, dev);

	    /*  alloc for the next   */
	    skb = dev_alloc_skb (1584 + 2);
	    if (skb == NULL) {
		printk ("xlan_intr: Memory squeeze, deferring packet\n");
		xlan_stats.rx_dropped++;
		goto tail;
	    }

	    skb_reserve (skb, 2);       /*  16 byte align  */

	    netif_rx (lan_inskb[i]);
	    lan_inskb[i] = skb;
	    lan_inbuf[i] = (struct lan_indata *) &skb->data[-4];
					/*  there are 16 + 2 bytes reserved  */

#else  /*  ! XLANC_DMA_RX   */

	    skb = dev_alloc_skb (ldp->len + 2);
	    if (skb == NULL) {
		printk ("xlan_intr: Memory squeeze, deferring packet\n");
		xlan_stats.rx_dropped++;
		goto tail;
	    }

	    skb->dev = dev;
	    skb_reserve (skb, 2);       /*  16 byte align for useful data   */
	    memcpy (skb_put (skb, ldp->len), &ldp->dest_addr, ldp->len);
	    skb->ip_summed = CHECKSUM_UNNECESSARY;
	    skb->protocol = eth_type_trans (skb, dev);

	    netif_rx (skb);
#endif  /*  XLANC_DMA_RX   */

	    xlan_stats.rx_packets++;

    tail:
	    i++;
	    if (i == RX_NUM_BUF)  i = 0;
	    v->xa = i;

	}  /*  while ( ... )   */

	if ((signed char) v->x0 < 0) {  /*  Tx interrupt   */
	    if ((v->x0 & 0x7f) != 0) {
		if (v->x13 & 0x40  &&  xlconn == 0) {
		    xlconn = 1;
		    xlan_stats.tx_errors++;
		    printk ("xlan_intr: network cable problem\n");
		}
	    } else xlconn = 0;

	    v->x0 = 0;

	    xlan_stats.tx_packets++;

#ifdef XLANC_DMA_TX
	    if (lan_outring[start_tx].skb) {
		dev_kfree_skb (lan_outring[start_tx].skb, FREE_WRITE);
		lan_outring[start_tx].skb = NULL;
	    }
#endif

	    start_tx++;
	    num_tx--;
	    if (start_tx == TX_NUM_BUF)  start_tx = 0;

	    if (dev->tbusy) {
		dev->tbusy = 0;
		mark_bh (NET_BH);
	    }

#ifdef XLANC_DMA_TX
	    if (num_tx != 0) {
		v->xc = lan_outring[start_tx].addr;
		v->x10 = lan_outring[start_tx].len;

		v->x0 = 66;

		dev->trans_start = jiffies;
	    }

#else  /*  ! XLANC_DMA_TX   */

	    if (num_tx != 0) {
		v->xc = &lan_outbuf[start_tx];
		v->x10 = lan_outlen[start_tx];

		v->x0 = 66;

		dev->trans_start = jiffies;
	    }
#endif  /*  XLANC_DMA_TX   */
	}

	dev->interrupt = 0;

	return;
}

static struct enet_statistics *xlan_get_stats (struct device *dev) {

	return &xlan_stats;
}

static int xlan_start_xmit (struct sk_buff *skb, struct device *dev) {
	volatile struct xlan *v = (struct xlan *) XLAN_ADDR;
	unsigned long flags;

	if (num_tx == TX_NUM_BUF) {
	    /* low level queue is still full  */
	    dev->tbusy = 1;     /*  paranoia   */

	    if (jiffies - dev->trans_start < 20)  return (1);

	    printk ("xlan: transmit timed out, retiming\n");
	    /*  may be restart the adaptor hear and clear dev->tbusy */
	    xlan_stats.tx_errors++;
	    dev->trans_start = jiffies;

	    return (1);
	}

	if (skb == NULL) {
	    dev_tint (dev);
	    return (0);
	}

	if (skb->len <= 0)  return (0);

	if (skb->len > 1582)  {
	    printk ("xlan: len %lx is too long, stripped\n", skb->len);
	    skb->len = 1582;
	}

#ifdef XLANC_DMA_TX
	save_flags (flags);
	cli();

	memcpy (skb->data + 6, skb->data, 6);
	lan_outring[end_tx].addr = skb->data + 6;
	lan_outring[end_tx].len =
			(skb->len > ETH_ZLEN ? skb->len : ETH_ZLEN) - 6;
	lan_outring[end_tx].skb = skb;
	end_tx++;
	if (end_tx == TX_NUM_BUF)  end_tx = 0;

	if (num_tx == 0) {

	    v->xc = lan_outring[start_tx].addr;
	    v->x10 = lan_outring[start_tx].len;

	    v->x0 = 66;

	    dev->trans_start = jiffies;
	}
	num_tx++;

	if (num_tx == TX_NUM_BUF)  dev->tbusy = 1;
	else  dev->tbusy = 0;

	restore_flags (flags);

#else  /*  ! XLANC_DMA_TX   */

	save_flags (flags);
	cli();

	memcpy (&lan_outbuf[end_tx], skb->data, 6);
	memcpy (&lan_outbuf[end_tx].sap, skb->data + 12, skb->len - 12);
	lan_outlen[end_tx] = (skb->len > ETH_ZLEN ? skb->len : ETH_ZLEN) - 6;
	end_tx++;
	if (end_tx == TX_NUM_BUF)  end_tx = 0;

	if (num_tx == 0) {
	    v->xc = &lan_outbuf[start_tx];
	    v->x10 = lan_outlen[start_tx];

	    v->x0 = 66;

	    dev->trans_start = jiffies;
	}
	num_tx++;

	if (num_tx == TX_NUM_BUF)  dev->tbusy = 1;
	else  dev->tbusy = 0;

	restore_flags (flags);

	dev_kfree_skb (skb, FREE_WRITE);

#endif  /*  XLANC_DMA_TX   */

	return (0);
}
