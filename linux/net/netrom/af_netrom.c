/*
 *	NET/ROM release 006
 *
 *	This code REQUIRES 1.3.0 or higher/ NET3.029
 *
 *	This module:
 *		This module is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 *
 *	History
 *	NET/ROM 001	Jonathan(G4KLX)	Cloned from the AX25 code.
 *	NET/ROM 002	Darryl(G7LED)	Fixes and address enhancement.
 *			Jonathan(G4KLX)	Complete bind re-think.
 *			Alan(GW4PTS)	Trivial tweaks into new format.
 *	NET/ROM	003	Jonathan(G4KLX)	Added G8BPQ extensions.
 *					Added NET/ROM routing ioctl.
 *			Darryl(G7LED)	Fix autobinding (on connect).
 *					Fixed nr_release(), set TCP_CLOSE, wakeup app
 *					context, THEN make the sock dead.
 *					Circuit ID check before allocating it on
 *					a connection.
 *			Alan(GW4PTS)	sendmsg/recvmsg only. Fixed connect clear bug
 *					inherited from AX.25
 *	NET/ROM 004	Jonathan(G4KLX)	Converted to module.
 */

#include <linux/config.h>
#if defined(CONFIG_NETROM) || defined(CONFIG_NETROM_MODULE)
#include <linux/module.h>
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
#include <linux/stat.h>
#include <net/ax25.h>
#include <linux/inet.h>
#include <linux/netdevice.h>
#include <linux/if_arp.h>
#include <linux/skbuff.h>
#include <net/sock.h>
#include <asm/segment.h>
#include <asm/system.h>
#include <linux/fcntl.h>
#include <linux/termios.h>	/* For TIOCINQ/OUTQ */
#include <linux/mm.h>
#include <linux/interrupt.h>
#include <linux/notifier.h>
#include <net/netrom.h>
#include <linux/proc_fs.h>
#include <net/ip.h>
#include <net/arp.h>
#include <linux/if_arp.h>

int sysctl_netrom_default_path_quality            = NR_DEFAULT_QUAL;
int sysctl_netrom_obsolescence_count_initialiser  = NR_DEFAULT_OBS;
int sysctl_netrom_network_ttl_initialiser         = NR_DEFAULT_TTL;
int sysctl_netrom_transport_timeout               = NR_DEFAULT_T1;
int sysctl_netrom_transport_maximum_tries         = NR_DEFAULT_N2;
int sysctl_netrom_transport_acknowledge_delay     = NR_DEFAULT_T2;
int sysctl_netrom_transport_busy_delay            = NR_DEFAULT_T4;
int sysctl_netrom_transport_requested_window_size = NR_DEFAULT_WINDOW;
int sysctl_netrom_routing_control                 = NR_DEFAULT_ROUTING;
int sysctl_netrom_link_fails_count                = NR_DEFAULT_FAILS;

static unsigned short circuit = 0x101;

static struct sock *volatile nr_list = NULL;

static void nr_free_sock(struct sock *sk)
{
	kfree_s(sk->protinfo.nr, sizeof(*sk->protinfo.nr));

	sk_free(sk);

	MOD_DEC_USE_COUNT;
}

static struct sock *nr_alloc_sock(void)
{
	struct sock *sk;
	nr_cb *nr;

	if ((sk = sk_alloc(GFP_ATOMIC)) == NULL)
		return NULL;

	if ((nr = (nr_cb *)kmalloc(sizeof(*nr), GFP_ATOMIC)) == NULL) {
		sk_free(sk);
		return NULL;
	}

	MOD_INC_USE_COUNT;

	memset(nr, 0x00, sizeof(*nr));

	sk->protinfo.nr = nr;
	nr->sk = sk;

	return sk;
}

/*
 *	Socket removal during an interrupt is now safe.
 */
static void nr_remove_socket(struct sock *sk)
{
	struct sock *s;
	unsigned long flags;

	save_flags(flags);
	cli();

	if ((s = nr_list) == sk) {
		nr_list = s->next;
		restore_flags(flags);
		return;
	}

	while (s != NULL && s->next != NULL) {
		if (s->next == sk) {
			s->next = sk->next;
			restore_flags(flags);
			return;
		}

		s = s->next;
	}

	restore_flags(flags);
}

/*
 *	Kill all bound sockets on a dropped device.
 */
static void nr_kill_by_device(struct device *dev)
{
	struct sock *s;

	for (s = nr_list; s != NULL; s = s->next) {
		if (s->protinfo.nr->device == dev) {
			s->protinfo.nr->state  = NR_STATE_0;
			s->protinfo.nr->device = NULL;
			s->state               = TCP_CLOSE;
			s->err                 = ENETUNREACH;
			s->shutdown           |= SEND_SHUTDOWN;
			s->state_change(s);
			s->dead                = 1;
		}
	}
}

/*
 *	Handle device status changes.
 */
static int nr_device_event(struct notifier_block *this, unsigned long event, void *ptr)
{
	struct device *dev = (struct device *)ptr;

	if (event != NETDEV_DOWN)
		return NOTIFY_DONE;

	nr_kill_by_device(dev);
	nr_rt_device_down(dev);
	
	return NOTIFY_DONE;
}

/*
 *	Add a socket to the bound sockets list.
 */
static void nr_insert_socket(struct sock *sk)
{
	unsigned long flags;

	save_flags(flags);
	cli();

	sk->next = nr_list;
	nr_list  = sk;

	restore_flags(flags);
}

/*
 *	Find a socket that wants to accept the Connect Request we just
 *	received.
 */
static struct sock *nr_find_listener(ax25_address *addr)
{
	unsigned long flags;
	struct sock *s;

	save_flags(flags);
	cli();

	for (s = nr_list; s != NULL; s = s->next) {
		if (ax25cmp(&s->protinfo.nr->source_addr, addr) == 0 && s->state == TCP_LISTEN) {
			restore_flags(flags);
			return s;
		}
	}

	restore_flags(flags);
	return NULL;
}

/*
 *	Find a connected NET/ROM socket given my circuit IDs.
 */
static struct sock *nr_find_socket(unsigned char index, unsigned char id)
{
	struct sock *s;
	unsigned long flags;

	save_flags(flags);
	cli();

	for (s = nr_list; s != NULL; s = s->next) {
		if (s->protinfo.nr->my_index == index && s->protinfo.nr->my_id == id) {
			restore_flags(flags);
			return s;
		}
	}

	restore_flags(flags);

	return NULL;
}

/*
 *	Find a connected NET/ROM socket given their circuit IDs.
 */
static struct sock *nr_find_peer(unsigned char index, unsigned char id, ax25_address *dest)
{
	struct sock *s;
	unsigned long flags;

	save_flags(flags);
	cli();

	for (s = nr_list; s != NULL; s = s->next) {
		if (s->protinfo.nr->your_index == index && s->protinfo.nr->your_id == id && ax25cmp(&s->protinfo.nr->dest_addr, dest) == 0) {
			restore_flags(flags);
			return s;
		}
	}

	restore_flags(flags);

	return NULL;
}

/*
 *	Find next free circuit ID.
 */
static unsigned short nr_find_next_circuit(void)
{
	unsigned short id = circuit;
	unsigned char i, j;

	while (1) {
		i = id / 256;
		j = id % 256;

		if (i != 0 && j != 0)
			if (nr_find_socket(i, j) == NULL)
				break;

		id++;
	}

	return id;
}

/*
 *	Deferred destroy.
 */
void nr_destroy_socket(struct sock *);

/*
 *	Handler for deferred kills.
 */
static void nr_destroy_timer(unsigned long data)
{
	nr_destroy_socket((struct sock *)data);
}

/*
 *	This is called from user mode and the timers. Thus it protects itself against
 *	interrupt users but doesn't worry about being called during work.
 *	Once it is removed from the queue no interrupt or bottom half will
 *	touch it and we are (fairly 8-) ) safe.
 */
void nr_destroy_socket(struct sock *sk)	/* Not static as it's used by the timer */
{
	struct sk_buff *skb;
	unsigned long flags;

	save_flags(flags);
	cli();

	del_timer(&sk->timer);

	nr_remove_socket(sk);
	nr_clear_queues(sk);		/* Flush the queues */

	while ((skb = skb_dequeue(&sk->receive_queue)) != NULL) {
		if (skb->sk != sk) {			/* A pending connection */
			skb->sk->dead = 1;	/* Queue the unaccepted socket for death */
			nr_set_timer(skb->sk);
			skb->sk->protinfo.nr->state = NR_STATE_0;
		}

		kfree_skb(skb, FREE_READ);
	}

	if (sk->wmem_alloc != 0 || sk->rmem_alloc != 0) {	/* Defer: outstanding buffers */
		init_timer(&sk->timer);
		sk->timer.expires  = jiffies + 10 * HZ;
		sk->timer.function = nr_destroy_timer;
		sk->timer.data     = (unsigned long)sk;
		add_timer(&sk->timer);
	} else {
		nr_free_sock(sk);
	}

	restore_flags(flags);
}

/*
 *	Handling for system calls applied via the various interfaces to a
 *	NET/ROM socket object.
 */
static int nr_fcntl(struct socket *sock, unsigned int cmd, unsigned long arg)
{
	return -EINVAL;
}

static int nr_setsockopt(struct socket *sock, int level, int optname,
	char *optval, int optlen)
{
	struct sock *sk = (struct sock *)sock->data;
	int err, opt;

	if (level == SOL_SOCKET)
		return sock_setsockopt(sk, level, optname, optval, optlen);

	if (level != SOL_NETROM)
		return -EOPNOTSUPP;

	if (optval == NULL)
		return -EINVAL;

	if ((err = verify_area(VERIFY_READ, optval, sizeof(int))) != 0)
		return err;

	opt = get_fs_long((unsigned long *)optval);

	switch (optname) {
		case NETROM_T1:
			if (opt < 1)
				return -EINVAL;
			sk->protinfo.nr->t1 = opt * NR_SLOWHZ;
			return 0;

		case NETROM_T2:
			if (opt < 1)
				return -EINVAL;
			sk->protinfo.nr->t2 = opt * NR_SLOWHZ;
			return 0;

		case NETROM_N2:
			if (opt < 1 || opt > 31)
				return -EINVAL;
			sk->protinfo.nr->n2 = opt;
			return 0;

		case NETROM_T4:
			if (opt < 1)
				return -EINVAL;
			sk->protinfo.nr->t4 = opt * NR_SLOWHZ;
			return 0;

		case NETROM_IDLE:
			if (opt < 0)
				return -EINVAL;
			return 0;

		default:
			return -ENOPROTOOPT;
	}
}

static int nr_getsockopt(struct socket *sock, int level, int optname,
	char *optval, int *optlen)
{
	struct sock *sk = (struct sock *)sock->data;
	int val = 0;
	int err; 

	if (level == SOL_SOCKET)
		return sock_getsockopt(sk, level, optname, optval, optlen);

	if (level != SOL_NETROM)
		return -EOPNOTSUPP;

	switch (optname) {
		case NETROM_T1:
			val = sk->protinfo.nr->t1 / NR_SLOWHZ;
			break;

		case NETROM_T2:
			val = sk->protinfo.nr->t2 / NR_SLOWHZ;
			break;

		case NETROM_N2:
			val = sk->protinfo.nr->n2;
			break;

		case NETROM_T4:
			val = sk->protinfo.nr->t4 / NR_SLOWHZ;
			break;

		case NETROM_IDLE:
			val = 0;
			break;

		default:
			return -ENOPROTOOPT;
	}

	if ((err = verify_area(VERIFY_WRITE, optlen, sizeof(int))) != 0)
		return err;

	put_fs_long(sizeof(int), (unsigned long *)optlen);

	if ((err = verify_area(VERIFY_WRITE, optval, sizeof(int))) != 0)
		return err;

	put_fs_long(val, (unsigned long *)optval);

	return 0;
}

static int nr_listen(struct socket *sock, int backlog)
{
	struct sock *sk = (struct sock *)sock->data;

	if (sk->state != TCP_LISTEN) {
		memset(&sk->protinfo.nr->user_addr, '\0', AX25_ADDR_LEN);
		sk->max_ack_backlog = backlog;
		sk->state           = TCP_LISTEN;
		return 0;
	}

	return -EOPNOTSUPP;
}

static void def_callback1(struct sock *sk)
{
	if (!sk->dead)
		wake_up_interruptible(sk->sleep);
}

static void def_callback2(struct sock *sk, int len)
{
	if (!sk->dead)
		wake_up_interruptible(sk->sleep);
}

static int nr_create(struct socket *sock, int protocol)
{
	struct sock *sk;
	nr_cb *nr;

	if (sock->type != SOCK_SEQPACKET || protocol != 0)
		return -ESOCKTNOSUPPORT;

	if ((sk = nr_alloc_sock()) == NULL)
		return -ENOMEM;

	nr = sk->protinfo.nr;

	skb_queue_head_init(&sk->receive_queue);
	skb_queue_head_init(&sk->write_queue);
	skb_queue_head_init(&sk->back_log);

	init_timer(&sk->timer);

	sk->socket        = sock;
	sk->type          = sock->type;
	sk->protocol      = protocol;
	sk->allocation	  = GFP_KERNEL;
	sk->rcvbuf        = SK_RMEM_MAX;
	sk->sndbuf        = SK_WMEM_MAX;
	sk->state         = TCP_CLOSE;
	sk->priority      = SOPRI_NORMAL;
	sk->mtu           = NETROM_MTU;	/* 236 */
	sk->zapped        = 1;

	sk->state_change = def_callback1;
	sk->data_ready   = def_callback2;
	sk->write_space  = def_callback1;
	sk->error_report = def_callback1;

	if (sock != NULL) {
		sock->data = (void *)sk;
		sk->sleep  = sock->wait;
	}

	skb_queue_head_init(&nr->ack_queue);
	skb_queue_head_init(&nr->reseq_queue);
	skb_queue_head_init(&nr->frag_queue);

	nr->t1     = sysctl_netrom_transport_timeout;
	nr->t2     = sysctl_netrom_transport_acknowledge_delay;
	nr->n2     = sysctl_netrom_transport_maximum_tries;
	nr->t4     = sysctl_netrom_transport_busy_delay;
	nr->window = sysctl_netrom_transport_requested_window_size;

	nr->bpqext = 1;
	nr->state  = NR_STATE_0;

	return 0;
}

static struct sock *nr_make_new(struct sock *osk)
{
	struct sock *sk;
	nr_cb *nr;

	if (osk->type != SOCK_SEQPACKET)
		return NULL;

	if ((sk = nr_alloc_sock()) == NULL)
		return NULL;

	nr = sk->protinfo.nr;

	skb_queue_head_init(&sk->receive_queue);
	skb_queue_head_init(&sk->write_queue);
	skb_queue_head_init(&sk->back_log);

	init_timer(&sk->timer);

	sk->type        = osk->type;
	sk->socket      = osk->socket;
	sk->priority    = osk->priority;
	sk->protocol    = osk->protocol;
	sk->rcvbuf      = osk->rcvbuf;
	sk->sndbuf      = osk->sndbuf;
	sk->debug       = osk->debug;
	sk->state       = TCP_ESTABLISHED;
	sk->mtu         = osk->mtu;
	sk->sleep       = osk->sleep;
	sk->zapped      = osk->zapped;

	sk->state_change = def_callback1;
	sk->data_ready   = def_callback2;
	sk->write_space  = def_callback1;
	sk->error_report = def_callback1;

	skb_queue_head_init(&nr->ack_queue);
	skb_queue_head_init(&nr->reseq_queue);
	skb_queue_head_init(&nr->frag_queue);

	nr->t1     = osk->protinfo.nr->t1;
	nr->t2     = osk->protinfo.nr->t2;
	nr->n2     = osk->protinfo.nr->n2;
	nr->t4     = osk->protinfo.nr->t4;
	nr->window = osk->protinfo.nr->window;

	nr->device  = osk->protinfo.nr->device;
	nr->bpqext  = osk->protinfo.nr->bpqext;

	return sk;
}

static int nr_dup(struct socket *newsock, struct socket *oldsock)
{
	struct sock *sk = (struct sock *)oldsock->data;

	if (sk == NULL || newsock == NULL)
		return -EINVAL;

	return nr_create(newsock, sk->protocol);
}

static int nr_release(struct socket *sock, struct socket *peer)
{
	struct sock *sk = (struct sock *)sock->data;

	if (sk == NULL) return 0;

	switch (sk->protinfo.nr->state) {

		case NR_STATE_0:
			sk->state     = TCP_CLOSE;
			sk->shutdown |= SEND_SHUTDOWN;
			sk->state_change(sk);
			sk->dead      = 1;
			nr_destroy_socket(sk);
			break;

                case NR_STATE_1:
			sk->protinfo.nr->state = NR_STATE_0;
			sk->state              = TCP_CLOSE;
			sk->shutdown          |= SEND_SHUTDOWN;
                        sk->state_change(sk);
			sk->dead               = 1;
			nr_destroy_socket(sk);
			break;

		case NR_STATE_2:
			nr_write_internal(sk, NR_DISCACK);
			sk->protinfo.nr->state = NR_STATE_0;
			sk->state              = TCP_CLOSE;
			sk->shutdown          |= SEND_SHUTDOWN;
			sk->state_change(sk);
			sk->dead               = 1;
			nr_destroy_socket(sk);
			break;			

		case NR_STATE_3:
			nr_clear_queues(sk);
			sk->protinfo.nr->n2count = 0;
			nr_write_internal(sk, NR_DISCREQ);
			sk->protinfo.nr->t1timer = sk->protinfo.nr->t1;
			sk->protinfo.nr->t2timer = 0;
			sk->protinfo.nr->t4timer = 0;
			sk->protinfo.nr->state   = NR_STATE_2;
			sk->state                = TCP_CLOSE;
			sk->shutdown            |= SEND_SHUTDOWN;
			sk->state_change(sk);
			sk->dead                 = 1;
			sk->destroy              = 1;
			break;

		default:
			break;
	}

	sock->data = NULL;	
	sk->socket = NULL;	/* Not used, but we should do this. **/

	return 0;
}

static int nr_bind(struct socket *sock, struct sockaddr *uaddr, int addr_len)
{
	struct sock *sk = (struct sock *)sock->data;
	struct full_sockaddr_ax25 *addr = (struct full_sockaddr_ax25 *)uaddr;
	struct device *dev;
	ax25_address *user, *source;

	if (sk->zapped == 0)
		return -EINVAL;

	if (addr_len != sizeof(struct sockaddr_ax25) && addr_len != sizeof(struct full_sockaddr_ax25))
		return -EINVAL;

	if (addr->fsa_ax25.sax25_family != AF_NETROM)
		return -EINVAL;

	if ((dev = nr_dev_get(&addr->fsa_ax25.sax25_call)) == NULL) {
		if (sk->debug)
			printk("NET/ROM: bind failed: invalid node callsign\n");
		return -EADDRNOTAVAIL;
	}

	/*
	 * Only the super user can set an arbitrary user callsign.
	 */
	if (addr->fsa_ax25.sax25_ndigis == 1) {
		if (!suser())
			return -EACCES;
		sk->protinfo.nr->user_addr   = addr->fsa_digipeater[0];
		sk->protinfo.nr->source_addr = addr->fsa_ax25.sax25_call;
	} else {
		source = &addr->fsa_ax25.sax25_call;

		if ((user = ax25_findbyuid(current->euid)) == NULL) {
			if (ax25_uid_policy && !suser())
				return -EPERM;
			user = source;
		}

		sk->protinfo.nr->user_addr   = *user;
		sk->protinfo.nr->source_addr = *source;
	}

	sk->protinfo.nr->device = dev;
	nr_insert_socket(sk);

	sk->zapped = 0;

	if (sk->debug)
		printk("NET/ROM: socket is bound\n");

	return 0;
}

static int nr_connect(struct socket *sock, struct sockaddr *uaddr,
	int addr_len, int flags)
{
	struct sock *sk = (struct sock *)sock->data;
	struct sockaddr_ax25 *addr = (struct sockaddr_ax25 *)uaddr;
	ax25_address *user, *source = NULL;
	struct device *dev;

	if (sk->state == TCP_ESTABLISHED && sock->state == SS_CONNECTING) {
		sock->state = SS_CONNECTED;
		return 0;	/* Connect completed during a ERESTARTSYS event */
	}

	if (sk->state == TCP_CLOSE && sock->state == SS_CONNECTING) {
		sock->state = SS_UNCONNECTED;
		return -ECONNREFUSED;
	}

	if (sk->state == TCP_ESTABLISHED)
		return -EISCONN;	/* No reconnect on a seqpacket socket */

	sk->state   = TCP_CLOSE;	
	sock->state = SS_UNCONNECTED;

	if (addr_len != sizeof(struct sockaddr_ax25) && addr_len != sizeof(struct full_sockaddr_ax25))
		return -EINVAL;

	if (addr->sax25_family != AF_NETROM)
		return -EINVAL;

	if (sk->zapped) {	/* Must bind first - autobinding in this may or may not work */
		sk->zapped = 0;

		if ((dev = nr_dev_first()) == NULL)
			return -ENETUNREACH;

		source = (ax25_address *)dev->dev_addr;

		if ((user = ax25_findbyuid(current->euid)) == NULL) {
			if (ax25_uid_policy && !suser())
				return -EPERM;
			user = source;
		}

		sk->protinfo.nr->user_addr   = *user;
		sk->protinfo.nr->source_addr = *source;
		sk->protinfo.nr->device      = dev;

		nr_insert_socket(sk);		/* Finish the bind */
	}

	sk->protinfo.nr->dest_addr = addr->sax25_call;

	circuit = nr_find_next_circuit();

	sk->protinfo.nr->my_index = circuit / 256;
	sk->protinfo.nr->my_id    = circuit % 256;

	circuit++;

	/* Move to connecting socket, start sending Connect Requests */
	sock->state            = SS_CONNECTING;
	sk->state              = TCP_SYN_SENT;
	nr_establish_data_link(sk);
	sk->protinfo.nr->state = NR_STATE_1;
	nr_set_timer(sk);

	/* Now the loop */
	if (sk->state != TCP_ESTABLISHED && (flags & O_NONBLOCK))
		return -EINPROGRESS;
		
	cli();	/* To avoid races on the sleep */

	/*
	 * A Connect Ack with Choke or timeout or failed routing will go to closed.
	 */
	while (sk->state == TCP_SYN_SENT) {
		interruptible_sleep_on(sk->sleep);
		if (current->signal & ~current->blocked) {
			sti();
			return -ERESTARTSYS;
		}
	}

	if (sk->state != TCP_ESTABLISHED) {
		sti();
		sock->state = SS_UNCONNECTED;
		return sock_error(sk);	/* Always set at this point */
	}

	sock->state = SS_CONNECTED;

	sti();

	return 0;
}

static int nr_socketpair(struct socket *sock1, struct socket *sock2)
{
	return -EOPNOTSUPP;
}

static int nr_accept(struct socket *sock, struct socket *newsock, int flags)
{
	struct sock *sk;
	struct sock *newsk;
	struct sk_buff *skb;

	if (newsock->data != NULL)
		nr_destroy_socket((struct sock *)newsock->data);

	newsock->data = NULL;

	if ((sk = (struct sock *)sock->data) == NULL)
		return -EINVAL;

	if (sk->type != SOCK_SEQPACKET)
		return -EOPNOTSUPP;

	if (sk->state != TCP_LISTEN)
		return -EINVAL;

	/*
	 *	The write queue this time is holding sockets ready to use
	 *	hooked into the SABM we saved
	 */
	do {
		cli();
		if ((skb = skb_dequeue(&sk->receive_queue)) == NULL) {
			if (flags & O_NONBLOCK) {
				sti();
				return -EWOULDBLOCK;
			}
			interruptible_sleep_on(sk->sleep);
			if (current->signal & ~current->blocked) {
				sti();
				return -ERESTARTSYS;
			}
		}
	} while (skb == NULL);

	newsk = skb->sk;
	newsk->pair = NULL;
	newsk->socket = newsock;
	newsk->sleep = newsock->wait;
	sti();

	/* Now attach up the new socket */
	skb->sk = NULL;
	kfree_skb(skb, FREE_READ);
	sk->ack_backlog--;
	newsock->data = newsk;

	return 0;
}

static int nr_getname(struct socket *sock, struct sockaddr *uaddr,
	int *uaddr_len, int peer)
{
	struct full_sockaddr_ax25 *sax = (struct full_sockaddr_ax25 *)uaddr;
	struct sock *sk = (struct sock *)sock->data;

	if (peer != 0) {
		if (sk->state != TCP_ESTABLISHED)
			return -ENOTCONN;
		sax->fsa_ax25.sax25_family = AF_NETROM;
		sax->fsa_ax25.sax25_ndigis = 1;
		sax->fsa_ax25.sax25_call   = sk->protinfo.nr->user_addr;
		sax->fsa_digipeater[0]     = sk->protinfo.nr->dest_addr;
		*uaddr_len = sizeof(struct full_sockaddr_ax25);
	} else {
		sax->fsa_ax25.sax25_family = AF_NETROM;
		sax->fsa_ax25.sax25_ndigis = 0;
		sax->fsa_ax25.sax25_call   = sk->protinfo.nr->source_addr;
		*uaddr_len = sizeof(struct sockaddr_ax25);
	}

	return 0;
}

int nr_rx_frame(struct sk_buff *skb, struct device *dev)
{
	struct sock *sk;
	struct sock *make;	
	ax25_address *src, *dest, *user;
	unsigned short circuit_index, circuit_id;
	unsigned short peer_circuit_index, peer_circuit_id;
	unsigned short frametype, flags, window, timeout;

	skb->sk = NULL;		/* Initially we don't know who it's for */

	/*
	 *	skb->data points to the netrom frame start
	 */

	src  = (ax25_address *)(skb->data + 0);
	dest = (ax25_address *)(skb->data + 7);

	circuit_index      = skb->data[15];
	circuit_id         = skb->data[16];
	peer_circuit_index = skb->data[17];
	peer_circuit_id    = skb->data[18];
	frametype          = skb->data[19] & 0x0F;
	flags              = skb->data[19] & 0xF0;

#ifdef CONFIG_INET
	/*
	 * Check for an incoming IP over NET/ROM frame.
	 */
	if (frametype == NR_PROTOEXT && circuit_index == NR_PROTO_IP && circuit_id == NR_PROTO_IP) {
		skb_pull(skb, NR_NETWORK_LEN + NR_TRANSPORT_LEN);
		skb->h.raw = skb->data;

		return nr_rx_ip(skb, dev);
	}
#endif

	/*
	 * Find an existing socket connection, based on circuit ID, if it's
	 * a Connect Request base it on their circuit ID.
	 *
	 * Circuit ID 0/0 is not valid but it could still be a "reset" for
	 * a circuit that no longer exists at the other end...
	 */

	sk = NULL;

	if (circuit_index == 0 && circuit_id == 0) {
		if (frametype == NR_CONNACK && flags == NR_CHOKE_FLAG)
			sk = nr_find_peer(peer_circuit_index, peer_circuit_id, src);
	} else {
		if (frametype == NR_CONNREQ)
			sk = nr_find_peer(circuit_index, circuit_id, src);
		else
			sk = nr_find_socket(circuit_index, circuit_id);
	}

	if (sk != NULL) {
		skb->h.raw = skb->data;

		if (frametype == NR_CONNACK && skb->len == 22)
			sk->protinfo.nr->bpqext = 1;
		else
			sk->protinfo.nr->bpqext = 0;

		return nr_process_rx_frame(sk, skb);
	}

	/*
	 * Now it should be a CONNREQ.
	 */
	if (frametype != NR_CONNREQ) {
		/*
		 * Never reply to a CONNACK/CHOKE.
		 */
		if (frametype != NR_CONNACK || flags != NR_CHOKE_FLAG)
			nr_transmit_dm(skb, 1);
		return 0;
	}

	sk = nr_find_listener(dest);

	user = (ax25_address *)(skb->data + 21);

	if (sk == NULL || sk->ack_backlog == sk->max_ack_backlog || (make = nr_make_new(sk)) == NULL) {
		nr_transmit_dm(skb, 0);
		return 0;
	}

	window = skb->data[20];

	skb->sk             = make;
	make->state         = TCP_ESTABLISHED;

	/* Fill in his circuit details */
	make->protinfo.nr->source_addr = *dest;
	make->protinfo.nr->dest_addr   = *src;
	make->protinfo.nr->user_addr   = *user;

	make->protinfo.nr->your_index  = circuit_index;
	make->protinfo.nr->your_id     = circuit_id;

	circuit = nr_find_next_circuit();

	make->protinfo.nr->my_index    = circuit / 256;
	make->protinfo.nr->my_id       = circuit % 256;

	circuit++;

	/* Window negotiation */
	if (window < make->protinfo.nr->window)
		make->protinfo.nr->window = window;

	/* L4 timeout negotiation */
	if (skb->len == 37) {
		timeout = skb->data[36] * 256 + skb->data[35];
		if (timeout * NR_SLOWHZ < make->protinfo.nr->t1)
			make->protinfo.nr->t1 = timeout * NR_SLOWHZ;
		make->protinfo.nr->bpqext = 1;
	} else {
		make->protinfo.nr->bpqext = 0;
	}

	nr_write_internal(make, NR_CONNACK);

	make->protinfo.nr->condition = 0x00;
	make->protinfo.nr->vs        = 0;
	make->protinfo.nr->va        = 0;
	make->protinfo.nr->vr        = 0;
	make->protinfo.nr->vl        = 0;
	make->protinfo.nr->state     = NR_STATE_3;
	sk->ack_backlog++;
	make->pair = sk;

	nr_insert_socket(make);

	skb_queue_head(&sk->receive_queue, skb);

	nr_set_timer(make);

	if (!sk->dead)
		sk->data_ready(sk, skb->len);

	return 1;
}

static int nr_sendmsg(struct socket *sock, struct msghdr *msg, int len, int noblock, int flags)
{
	struct sock *sk = (struct sock *)sock->data;
	struct sockaddr_ax25 *usax = (struct sockaddr_ax25 *)msg->msg_name;
	int err;
	struct sockaddr_ax25 sax;
	struct sk_buff *skb;
	unsigned char *asmptr;
	int size;

	if (sk->err)
		return sock_error(sk);

	if (flags)
		return -EINVAL;

	if (sk->zapped)
		return -EADDRNOTAVAIL;

	if (sk->shutdown & SEND_SHUTDOWN) {
		send_sig(SIGPIPE, current, 0);
		return -EPIPE;
	}

	if (sk->protinfo.nr->device == NULL)
		return -ENETUNREACH;

	if (usax) {
		if (msg->msg_namelen < sizeof(sax))
			return -EINVAL;
		sax = *usax;
		if (ax25cmp(&sk->protinfo.nr->dest_addr, &sax.sax25_call) != 0)
			return -EISCONN;
		if (sax.sax25_family != AF_NETROM)
			return -EINVAL;
	} else {
		if (sk->state != TCP_ESTABLISHED)
			return -ENOTCONN;
		sax.sax25_family = AF_NETROM;
		sax.sax25_call   = sk->protinfo.nr->dest_addr;
	}

	if (sk->debug)
		printk("NET/ROM: sendto: Addresses built.\n");

	/* Build a packet */
	if (sk->debug)
		printk("NET/ROM: sendto: building packet.\n");

	size = len + AX25_BPQ_HEADER_LEN + AX25_MAX_HEADER_LEN + NR_NETWORK_LEN + NR_TRANSPORT_LEN;

	if ((skb = sock_alloc_send_skb(sk, size, 0, 0, &err)) == NULL)
		return err;

	skb->sk   = sk;
	skb->free = 1;

	skb_reserve(skb, size - len);

	/*
	 *	Push down the NET/ROM header
	 */

	asmptr = skb_push(skb, NR_TRANSPORT_LEN);

	if (sk->debug)
		printk("Building NET/ROM Header.\n");

	/* Build a NET/ROM Transport header */

	*asmptr++ = sk->protinfo.nr->your_index;
	*asmptr++ = sk->protinfo.nr->your_id;
	*asmptr++ = 0;		/* To be filled in later */
	*asmptr++ = 0;		/*      Ditto            */
	*asmptr++ = NR_INFO;

	if (sk->debug)
		printk("Built header.\n");

	/*
	 *	Put the data on the end
	 */

	skb->h.raw = skb_put(skb, len);

	asmptr = skb->h.raw;

	if (sk->debug)
		printk("NET/ROM: Appending user data\n");

	/* User data follows immediately after the NET/ROM transport header */
	memcpy_fromiovec(asmptr, msg->msg_iov, len);

	if (sk->debug)
		printk("NET/ROM: Transmitting buffer\n");

	if (sk->state != TCP_ESTABLISHED) {
		kfree_skb(skb, FREE_WRITE);
		return -ENOTCONN;
	}

	nr_output(sk, skb);	/* Shove it onto the queue */

	return len;
}


static int nr_recvmsg(struct socket *sock, struct msghdr *msg, int size, int noblock,
		   int flags, int *addr_len)
{
	struct sock *sk = (struct sock *)sock->data;
	struct sockaddr_ax25 *sax = (struct sockaddr_ax25 *)msg->msg_name;
	int copied;
	struct sk_buff *skb;
	int er;

	if (sk->err)
		return sock_error(sk);
	
	if (addr_len != NULL)
		*addr_len = sizeof(*sax);

	/*
	 * This works for seqpacket too. The receiver has ordered the queue for
	 * us! We do one quick check first though
	 */

	if (sk->state != TCP_ESTABLISHED)
		return -ENOTCONN;

	/* Now we can treat all alike */
	if ((skb = skb_recv_datagram(sk, flags, noblock, &er)) == NULL)
		return er;

	skb->h.raw = skb->data;

	copied = (size < skb->len) ? size : skb->len;
	skb_copy_datagram_iovec(skb, 0, msg->msg_iov, copied);

	if (sax != NULL) {
		sax->sax25_family = AF_NETROM;
		memcpy(&sax->sax25_call, skb->data + 7, AX25_ADDR_LEN);

		*addr_len = sizeof(*sax);
	}

	skb_free_datagram(sk, skb);

	return copied;
}

static int nr_shutdown(struct socket *sk, int how)
{
	return -EOPNOTSUPP;
}

static int nr_select(struct socket *sock , int sel_type, select_table *wait)
{
	struct sock *sk = (struct sock *)sock->data;

	return datagram_select(sk, sel_type, wait);
}

static int nr_ioctl(struct socket *sock, unsigned int cmd, unsigned long arg)
{
	struct sock *sk = (struct sock *)sock->data;
	int err;

	switch (cmd) {
		case TIOCOUTQ: {
			long amount;
			if ((err = verify_area(VERIFY_WRITE, (void *)arg, sizeof(int))) != 0)
				return err;
			amount = sk->sndbuf - sk->wmem_alloc;
			if (amount < 0)
				amount = 0;
			put_fs_long(amount, (int *)arg);
			return 0;
		}

		case TIOCINQ: {
			struct sk_buff *skb;
			long amount = 0L;
			/* These two are safe on a single CPU system as only user tasks fiddle here */
			if ((skb = skb_peek(&sk->receive_queue)) != NULL)
				amount = skb->len;
			if ((err = verify_area(VERIFY_WRITE, (void *)arg, sizeof(int))) != 0)
				return err;
			put_fs_long(amount, (int *)arg);
			return 0;
		}

		case SIOCGSTAMP:
			if (sk != NULL) {
				if (sk->stamp.tv_sec == 0)
					return -ENOENT;
				if ((err = verify_area(VERIFY_WRITE, (void *)arg, sizeof(struct timeval))) != 0)
					return err;
				memcpy_tofs((void *)arg, &sk->stamp, sizeof(struct timeval));
				return 0;
			}
			return -EINVAL;

		case SIOCGIFADDR:
		case SIOCSIFADDR:
		case SIOCGIFDSTADDR:
		case SIOCSIFDSTADDR:
		case SIOCGIFBRDADDR:
		case SIOCSIFBRDADDR:
		case SIOCGIFNETMASK:
		case SIOCSIFNETMASK:
		case SIOCGIFMETRIC:
		case SIOCSIFMETRIC:
			return -EINVAL;

		case SIOCADDRT:
		case SIOCDELRT:
		case SIOCNRDECOBS:
			if (!suser()) return -EPERM;
			return nr_rt_ioctl(cmd, (void *)arg);

 		default:
			return dev_ioctl(cmd, (void *)arg);
	}

	/*NOTREACHED*/
	return 0;
}

static int nr_get_info(char *buffer, char **start, off_t offset, int length, int dummy)
{
	struct sock *s;
	struct device *dev;
	const char *devname;
	int len = 0;
	off_t pos = 0;
	off_t begin = 0;

	cli();

	len += sprintf(buffer, "user_addr dest_node src_node  dev    my  your  st  vs  vr  va    t1     t2     t4      idle   n2  wnd Snd-Q Rcv-Q Inode\n");

	for (s = nr_list; s != NULL; s = s->next) {
		if ((dev = s->protinfo.nr->device) == NULL)
			devname = "???";
		else
			devname = dev->name;

		len += sprintf(buffer + len, "%-9s ",
			ax2asc(&s->protinfo.nr->user_addr));
		len += sprintf(buffer + len, "%-9s ",
			ax2asc(&s->protinfo.nr->dest_addr));
		len += sprintf(buffer + len, "%-9s %-3s  %02X/%02X %02X/%02X %2d %3d %3d %3d %3d/%03d %2d/%02d %3d/%03d %3d/%03d %2d/%02d %3d %5d %5d %ld\n",
			ax2asc(&s->protinfo.nr->source_addr),
			devname, s->protinfo.nr->my_index, s->protinfo.nr->my_id,
			s->protinfo.nr->your_index, s->protinfo.nr->your_id,
			s->protinfo.nr->state,
			s->protinfo.nr->vs, s->protinfo.nr->vr, s->protinfo.nr->va,
			s->protinfo.nr->t1timer / NR_SLOWHZ,
			s->protinfo.nr->t1      / NR_SLOWHZ,
			s->protinfo.nr->t2timer / NR_SLOWHZ,
			s->protinfo.nr->t2      / NR_SLOWHZ,
			s->protinfo.nr->t4timer / NR_SLOWHZ,
			s->protinfo.nr->t4      / NR_SLOWHZ,
			0,
			0,
			s->protinfo.nr->n2count,
			s->protinfo.nr->n2,
			s->protinfo.nr->window,
			s->wmem_alloc, s->rmem_alloc,
			s->socket && SOCK_INODE(s->socket) ?
			SOCK_INODE(s->socket)->i_ino : 0);

		pos = begin + len;

		if (pos < offset) {
			len   = 0;
			begin = pos;
		}

		if (pos > offset + length)
			break;
	}

	sti();

	*start = buffer + (offset - begin);
	len   -= (offset - begin);

	if (len > length) len = length;

	return(len);
} 

static struct proto_ops nr_proto_ops = {
	AF_NETROM,

	nr_create,
	nr_dup,
	nr_release,
	nr_bind,
	nr_connect,
	nr_socketpair,
	nr_accept,
	nr_getname,
	nr_select,
	nr_ioctl,
	nr_listen,
	nr_shutdown,
	nr_setsockopt,
	nr_getsockopt,
	nr_fcntl,
	nr_sendmsg,
	nr_recvmsg
};

static struct notifier_block nr_dev_notifier = {
	nr_device_event,
	0
};

#ifdef CONFIG_PROC_FS
static struct proc_dir_entry proc_net_nr = {
	PROC_NET_NR, 2, "nr",
	S_IFREG | S_IRUGO, 1, 0, 0,
	0, &proc_net_inode_operations, 
	nr_get_info
};
static struct proc_dir_entry proc_net_nr_neigh = {
	PROC_NET_NR_NEIGH, 8, "nr_neigh",
	S_IFREG | S_IRUGO, 1, 0, 0,
	0, &proc_net_inode_operations, 
	nr_neigh_get_info
};
static struct proc_dir_entry proc_net_nr_nodes = {
	PROC_NET_NR_NODES, 8, "nr_nodes",
	S_IFREG | S_IRUGO, 1, 0, 0,
	0, &proc_net_inode_operations, 
	nr_nodes_get_info
};
#endif	

static struct device dev_nr[] = {
	{"nr0", 0, 0, 0, 0, 0, 0, 0, 0, 0, NULL, nr_init},
	{"nr1", 0, 0, 0, 0, 0, 0, 0, 0, 0, NULL, nr_init},
	{"nr2", 0, 0, 0, 0, 0, 0, 0, 0, 0, NULL, nr_init},
	{"nr3", 0, 0, 0, 0, 0, 0, 0, 0, 0, NULL, nr_init}
};

void nr_proto_init(struct net_proto *pro)
{
	int i;

	sock_register(nr_proto_ops.family, &nr_proto_ops);
	register_netdevice_notifier(&nr_dev_notifier);
	printk(KERN_INFO "G4KLX NET/ROM for Linux. Version 0.6 for AX25.035 Linux 2.0\n");

	if (!ax25_protocol_register(AX25_P_NETROM, nr_route_frame))
		printk(KERN_ERR "NET/ROM unable to register protocol with AX.25\n");
	if (!ax25_linkfail_register(nr_link_failed))
		printk(KERN_ERR "NET/ROM unable to register linkfail handler with AX.25\n");

	for (i = 0; i < 4; i++)
		register_netdev(&dev_nr[i]);

	nr_register_sysctl();

	nr_loopback_init();

#ifdef CONFIG_PROC_FS
	proc_net_register(&proc_net_nr);
	proc_net_register(&proc_net_nr_neigh);
	proc_net_register(&proc_net_nr_nodes);
#endif	
}

#ifdef MODULE

int init_module(void)
{
	nr_proto_init(NULL);

	register_symtab(NULL);

	return 0;
}

void cleanup_module(void)
{
	int i;

#ifdef CONFIG_PROC_FS
	proc_net_unregister(PROC_NET_NR);
	proc_net_unregister(PROC_NET_NR_NEIGH);
	proc_net_unregister(PROC_NET_NR_NODES);
#endif
	nr_loopback_clear();

	nr_rt_free();

	ax25_protocol_release(AX25_P_NETROM);
	ax25_linkfail_release(nr_link_failed);

	unregister_netdevice_notifier(&nr_dev_notifier);

	nr_unregister_sysctl();

	sock_unregister(AF_NETROM);

	for (i = 0; i < 4; i++) {
		if (dev_nr[i].priv != NULL) {
			kfree(dev_nr[i].priv);
			dev_nr[i].priv = NULL;
			unregister_netdev(&dev_nr[i]);
		}
	}
}

#endif

#endif
