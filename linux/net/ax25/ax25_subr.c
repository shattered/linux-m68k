/*
 *	AX.25 release 035
 *
 *	This code REQUIRES 1.3.61 or higher/ NET3.029
 *
 *	This module:
 *		This module is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 *
 *	Most of this code is based on the SDL diagrams published in the 7th
 *	ARRL Computer Networking Conference papers. The diagrams have mistakes
 *	in them, but are mostly correct. Before you modify the code could you
 *	read the SDL diagrams as the code is not obvious and probably very
 *	easy to break;
 *
 *	History
 *	AX.25 029	Alan(GW4PTS)	Switched to KA9Q constant names. Removed
 *					old BSD code.
 *	AX.25 030	Jonathan(G4KLX)	Added support for extended AX.25.
 *					Added fragmentation support.
 *			Darryl(G7LED)	Added function ax25_requeue_frames() to split
 *					it up from ax25_frames_acked().
 *	AX.25 031	Joerg(DL1BKE)	DAMA needs KISS Fullduplex ON/OFF.
 *					Thus we have ax25_kiss_cmd() now... ;-)
 *			Dave Brown(N2RJT)
 *					Killed a silly bug in the DAMA code.
 *			Joerg(DL1BKE)	Found the real bug in ax25.h, sri.
 *	AX.25 032	Joerg(DL1BKE)	Added ax25_queue_length to count the number of
 *					enqueued buffers of a socket..
 *	AX.25 035	Frederic(F1OAT)	Support for pseudo-digipeating.
 */

#include <linux/config.h>
#if defined(CONFIG_AX25) || defined(CONFIG_AX25_MODULE)
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/socket.h>
#include <linux/in.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/timer.h>
#include <linux/string.h>
#include <linux/sockios.h>
#include <linux/net.h>
#include <net/ax25.h>
#include <linux/inet.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <net/sock.h>
#include <asm/segment.h>
#include <asm/system.h>
#include <linux/fcntl.h>
#include <linux/mm.h>
#include <linux/interrupt.h>

/*
 *	This routine purges all the queues of frames.
 */
void ax25_clear_queues(ax25_cb *ax25)
{
	struct sk_buff *skb;

	while ((skb = skb_dequeue(&ax25->write_queue)) != NULL)
		kfree_skb(skb, FREE_WRITE);

	while ((skb = skb_dequeue(&ax25->ack_queue)) != NULL)
		kfree_skb(skb, FREE_WRITE);

	while ((skb = skb_dequeue(&ax25->reseq_queue)) != NULL)
		kfree_skb(skb, FREE_READ);

	while ((skb = skb_dequeue(&ax25->frag_queue)) != NULL)
		kfree_skb(skb, FREE_READ);
}

/*
 * This routine purges the input queue of those frames that have been
 * acknowledged. This replaces the boxes labelled "V(a) <- N(r)" on the
 * SDL diagram.
 */
void ax25_frames_acked(ax25_cb *ax25, unsigned short nr)
{
	struct sk_buff *skb;

	/*
	 * Remove all the ack-ed frames from the ack queue.
	 */
	if (ax25->va != nr) {
		while (skb_peek(&ax25->ack_queue) != NULL && ax25->va != nr) {
		        skb = skb_dequeue(&ax25->ack_queue);
			kfree_skb(skb, FREE_WRITE);
			ax25->va = (ax25->va + 1) % ax25->modulus;
			if (ax25->dama_slave)
				ax25->n2count = 0;
		}
	}
}

/*
 * Maybe this should be your ax25_invoke_retransmission(), which appears
 * to be used but not do anything.  ax25_invoke_retransmission() used to
 * be in AX 0.29, but has now gone in 0.30.
 */
void ax25_requeue_frames(ax25_cb *ax25)
{
        struct sk_buff *skb, *skb_prev = NULL;

	/*
	 * Requeue all the un-ack-ed frames on the output queue to be picked
	 * up by ax25_kick called from the timer. This arrangement handles the
	 * possibility of an empty output queue.
	 */
	while ((skb = skb_dequeue(&ax25->ack_queue)) != NULL) {
		if (skb_prev == NULL)
			skb_queue_head(&ax25->write_queue, skb);
		else
			skb_append(skb_prev, skb);
		skb_prev = skb;
	}
}

/*
 *	Validate that the value of nr is between va and vs. Return true or
 *	false for testing.
 */
int ax25_validate_nr(ax25_cb *ax25, unsigned short nr)
{
	unsigned short vc = ax25->va;

	while (vc != ax25->vs) {
		if (nr == vc) return 1;
		vc = (vc + 1) % ax25->modulus;
	}

	if (nr == ax25->vs) return 1;

	return 0;
}

/*
 *	This routine is the centralised routine for parsing the control
 *	information for the different frame formats.
 */
int ax25_decode(ax25_cb *ax25, struct sk_buff *skb, int *ns, int *nr, int *pf)
{
	unsigned char *frame;
	int frametype = AX25_ILLEGAL;

	frame = skb->data;
	*ns = *nr = *pf = 0;

	if (ax25->modulus == AX25_MODULUS) {
		if ((frame[0] & AX25_S) == 0) {
			frametype = AX25_I;			/* I frame - carries NR/NS/PF */
			*ns = (frame[0] >> 1) & 0x07;
			*nr = (frame[0] >> 5) & 0x07;
			*pf = frame[0] & AX25_PF;
		} else if ((frame[0] & AX25_U) == 1) { 	/* S frame - take out PF/NR */
			frametype = frame[0] & 0x0F;
			*nr = (frame[0] >> 5) & 0x07;
			*pf = frame[0] & AX25_PF;
		} else if ((frame[0] & AX25_U) == 3) { 	/* U frame - take out PF */
			frametype = frame[0] & ~AX25_PF;
			*pf = frame[0] & AX25_PF;
		}
		skb_pull(skb, 1);
	} else {
		if ((frame[0] & AX25_S) == 0) {
			frametype = AX25_I;			/* I frame - carries NR/NS/PF */
			*ns = (frame[0] >> 1) & 0x7F;
			*nr = (frame[1] >> 1) & 0x7F;
			*pf = frame[1] & AX25_EPF;
			skb_pull(skb, 2);
		} else if ((frame[0] & AX25_U) == 1) { 	/* S frame - take out PF/NR */
			frametype = frame[0] & 0x0F;
			*nr = (frame[1] >> 1) & 0x7F;
			*pf = frame[1] & AX25_EPF;
			skb_pull(skb, 2);
		} else if ((frame[0] & AX25_U) == 3) { 	/* U frame - take out PF */
			frametype = frame[0] & ~AX25_PF;
			*pf = frame[0] & AX25_PF;
			skb_pull(skb, 1);
		}
	}

	return frametype;
}

/* 
 *	This routine is called when the HDLC layer internally  generates a
 *	command or  response  for  the remote machine ( eg. RR, UA etc. ). 
 *	Only supervisory or unnumbered frames are processed.
 */
void ax25_send_control(ax25_cb *ax25, int frametype, int poll_bit, int type)
{
	struct sk_buff *skb;
	unsigned char  *dptr;
	struct device *dev;

	if ((dev = ax25->device) == NULL)
		return;	/* Route died */

	if ((skb = alloc_skb(AX25_BPQ_HEADER_LEN + size_ax25_addr(ax25->digipeat) + 2, GFP_ATOMIC)) == NULL)
		return;

	skb->free = 1;

	skb_reserve(skb, AX25_BPQ_HEADER_LEN + size_ax25_addr(ax25->digipeat));

	/* Assume a response - address structure for DTE */
	if (ax25->modulus == AX25_MODULUS) {
		dptr = skb_put(skb, 1);
		*dptr = frametype;
		*dptr |= (poll_bit) ? AX25_PF : 0;
		if ((frametype & AX25_U) == AX25_S)		/* S frames carry NR */
			*dptr |= (ax25->vr << 5);
	} else {
		if ((frametype & AX25_U) == AX25_U) {
			dptr = skb_put(skb, 1);
			*dptr = frametype;
			*dptr |= (poll_bit) ? AX25_PF : 0;
		} else {
			dptr = skb_put(skb, 2);
			dptr[0] = frametype;
			dptr[1] = (ax25->vr << 1);
			dptr[1] |= (poll_bit) ? AX25_EPF : 0;
		}
	}

	ax25_transmit_buffer(ax25, skb, type);
}

/*
 *	Send a 'DM' to an unknown connection attempt, or an invalid caller.
 *
 *	Note: src here is the sender, thus it's the target of the DM
 */
void ax25_return_dm(struct device *dev, ax25_address *src, ax25_address *dest, ax25_digi *digi)
{
	struct sk_buff *skb;
	char *dptr;
	ax25_digi retdigi;

	if (dev == NULL)
		return;

	if ((skb = alloc_skb(AX25_BPQ_HEADER_LEN + size_ax25_addr(digi) + 1, GFP_ATOMIC)) == NULL)
		return;	/* Next SABM will get DM'd */

	skb->free = 1;

	skb_reserve(skb, AX25_BPQ_HEADER_LEN + size_ax25_addr(digi));

	ax25_digi_invert(digi, &retdigi);

	dptr = skb_put(skb, 1);

	*dptr = AX25_DM | AX25_PF;

	/*
	 *	Do the address ourselves
	 */
	dptr  = skb_push(skb, size_ax25_addr(digi));
	dptr += build_ax25_addr(dptr, dest, src, &retdigi, AX25_RESPONSE, AX25_MODULUS);

	ax25_queue_xmit(skb, dev, SOPRI_NORMAL);
}

/*
 *	Exponential backoff for AX.25
 */
unsigned short ax25_calculate_t1(ax25_cb *ax25)
{
	int n, t = 2;

	switch (ax25->backoff) {
		case 0:
			break;

		case 1:
			t += 2 * ax25->n2count;
			break;

		case 2:
			for (n = 0; n < ax25->n2count; n++)
				t *= 2;
			if (t > 8) t = 8;
			break;
	}

	return t * ax25->rtt;
}

/*
 *	Calculate the Round Trip Time
 */
void ax25_calculate_rtt(ax25_cb *ax25)
{
	switch (ax25->backoff) {
		case 0:
			ax25->rtt = ax25->t1 / 2;
			break;

		case 1:
		case 2:
			if (ax25->t1timer > 0 && ax25->n2count == 0)
				ax25->rtt = (9 * ax25->rtt + ax25->t1 - ax25->t1timer) / 10;
			break;
	}

	if (ax25->rtt < AX25_T1CLAMPLO)
		ax25->rtt = AX25_T1CLAMPLO;

	if (ax25->rtt > AX25_T1CLAMPHI)
		ax25->rtt = AX25_T1CLAMPHI;
}

/*
 *	Digipeated address processing
 */


/*
 *	Given an AX.25 address pull of to, from, digi list, command/response and the start of data
 *
 */
unsigned char *ax25_parse_addr(unsigned char *buf, int len, ax25_address *src, ax25_address *dest, ax25_digi *digi, int *flags, int *dama)
{
	int d = 0;

	if (len < 14) return NULL;

	*flags = 0;

	if (buf[6] & AX25_CBIT)
		*flags = AX25_COMMAND;
	if (buf[13] & AX25_CBIT)
		*flags = AX25_RESPONSE;

	if (dama != NULL) 
		*dama = ~buf[13] & AX25_DAMA_FLAG;

	/* Copy to, from */
	memcpy(dest, buf + 0, AX25_ADDR_LEN);
	memcpy(src,  buf + 7, AX25_ADDR_LEN);

	buf += 2 * AX25_ADDR_LEN;
	len -= 2 * AX25_ADDR_LEN;

	digi->lastrepeat = -1;
	digi->ndigi      = 0;

	while (!(buf[-1] & AX25_EBIT)) {
		if (d >= AX25_MAX_DIGIS)  return NULL;	/* Max of 6 digis */
		if (len < 7) return NULL;	/* Short packet */

		memcpy(&digi->calls[d], buf, AX25_ADDR_LEN);
		digi->ndigi = d + 1;

		if (buf[6] & AX25_HBIT) {
			digi->repeated[d] = 1;
			digi->lastrepeat  = d;
		} else {
			digi->repeated[d] = 0;
		}

		buf += AX25_ADDR_LEN;
		len -= AX25_ADDR_LEN;
		d++;
	}

	return buf;
}

/*
 *	Assemble an AX.25 header from the bits
 */
int build_ax25_addr(unsigned char *buf, ax25_address *src, ax25_address *dest, ax25_digi *d, int flag, int modulus)
{
	int len = 0;
	int ct  = 0;

	memcpy(buf, dest, AX25_ADDR_LEN);
	buf[6] &= ~(AX25_EBIT | AX25_CBIT);
	buf[6] |= AX25_SSSID_SPARE;

	if (flag == AX25_COMMAND) buf[6] |= AX25_CBIT;

	buf += AX25_ADDR_LEN;
	len += AX25_ADDR_LEN;

	memcpy(buf, src, AX25_ADDR_LEN);
	buf[6] &= ~(AX25_EBIT | AX25_CBIT);
	buf[6] &= ~AX25_SSSID_SPARE;

	if (modulus == AX25_MODULUS)
		buf[6] |= AX25_SSSID_SPARE;
	else
		buf[6] |= AX25_ESSID_SPARE;

	if (flag == AX25_RESPONSE) buf[6] |= AX25_CBIT;

	/*
	 *	Fast path the normal digiless path
	 */
	if (d == NULL || d->ndigi == 0) {
		buf[6] |= AX25_EBIT;
		return 2 * AX25_ADDR_LEN;
	}	

	buf += AX25_ADDR_LEN;
	len += AX25_ADDR_LEN;

	while (ct < d->ndigi) {
		memcpy(buf, &d->calls[ct], AX25_ADDR_LEN);

		if (d->repeated[ct])
			buf[6] |= AX25_HBIT;
		else
			buf[6] &= ~AX25_HBIT;

		buf[6] &= ~AX25_EBIT;
		buf[6] |= AX25_SSSID_SPARE;

		buf += AX25_ADDR_LEN;
		len += AX25_ADDR_LEN;
		ct++;
	}

	buf[-1] |= AX25_EBIT;

	return len;
}

int size_ax25_addr(ax25_digi *dp)
{
	if (dp == NULL)
		return 2 * AX25_ADDR_LEN;

	return AX25_ADDR_LEN * (2 + dp->ndigi);
}

/* 
 *	Reverse Digipeat List. May not pass both parameters as same struct
 */
void ax25_digi_invert(ax25_digi *in, ax25_digi *out)
{
	int ct;

	out->ndigi      = in->ndigi;
	out->lastrepeat = in->ndigi - in->lastrepeat - 2;

	/* Invert the digipeaters */

	for (ct = 0; ct < in->ndigi; ct++) {
		out->calls[ct] = in->calls[in->ndigi - ct - 1];

		if (ct <= out->lastrepeat) {
			out->calls[ct].ax25_call[6] |= AX25_HBIT;
			out->repeated[ct]            = 1;
		} else {
			out->calls[ct].ax25_call[6] &= ~AX25_HBIT;
			out->repeated[ct]            = 0;
		}
	}
}

/*
 *	:::FIXME:::
 *	This is ****NOT**** the right approach. Not all drivers do kiss. We
 *	need a driver level request to switch duplex mode, that does either
 *	SCC changing, PI config or KISS as required.
 *
 *	Not to mention this request isn't currently reliable.
 */
void ax25_kiss_cmd(ax25_cb *ax25, unsigned char cmd, unsigned char param)
{
	struct sk_buff *skb;
	unsigned char *p;

	if (ax25->device == NULL)
		return;

	if ((skb = alloc_skb(2, GFP_ATOMIC)) == NULL)
		return;

	skb->free = 1;
	skb->arp  = 1;

	skb->protocol = htons(ETH_P_AX25);

	p = skb_put(skb, 2);

	*p++ = cmd;
	*p++ = param;

	dev_queue_xmit(skb, ax25->device, SOPRI_NORMAL);
}

void ax25_dama_on(ax25_cb *ax25)
{
	if (ax25_dev_is_dama_slave(ax25->device) == 0) {
		if (ax25->sk != NULL && ax25->sk->debug)
			printk("ax25_dama_on: DAMA on\n");

		ax25_kiss_cmd(ax25, 5, 1);
	}
}

void ax25_dama_off(ax25_cb *ax25)
{
	if (ax25->dama_slave == 0)
		return;

	ax25->dama_slave = 0;

	if (ax25_dev_is_dama_slave(ax25->device) == 0) {
		if (ax25->sk != NULL && ax25->sk->debug)
			printk("ax25_dama_off: DAMA off\n");

		ax25_kiss_cmd(ax25, 5, 0);
	}
}

void ax25_disconnect(ax25_cb *ax25, int reason)
{
	ax25_clear_queues(ax25);

	ax25_dama_off(ax25);

	ax25->t1timer   = 0;
	ax25->t2timer   = 0;
	ax25->t3timer   = 0;
	ax25->idletimer = 0;

	ax25->state = AX25_STATE_0;

	ax25_link_failed(ax25, reason);

	if (ax25->sk != NULL) {
		ax25->sk->state     = TCP_CLOSE;
		ax25->sk->err       = reason;
		ax25->sk->shutdown |= SEND_SHUTDOWN;
		if (!ax25->sk->dead)
			ax25->sk->state_change(ax25->sk);
		ax25->sk->dead      = 1;
	}
}

#endif
