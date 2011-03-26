/*
 * besta/sockmod.c -- To emulate sysv`s "sockmod" stream behavior under Linux.
 *		      A module to use with `besta/stream.c' services.
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
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/malloc.h>
#include <linux/kd.h>
#include <linux/sched.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/linkage.h>
#include <linux/kernel_stat.h>
#include <linux/major.h>
#include <linux/stat.h>
#include <linux/socket.h>
#include <linux/net.h>
#include <linux/in.h>

#include <asm/system.h>
#include <asm/page.h>

#include "stream.h"

#undef MIN
#define MIN(A,B)        ((A) < (B) ? (A) : (B));

#define INET_STREAM_MAJOR       56
#define INET_DGRAM_MAJOR        57
#define UNIX_STREAM_MAJOR       58
#define UNIX_DGRAM_MAJOR        59

/*  I have seen it is `private' in /etc/services . Now I don`t know
   what does it mean, but I hope  there are no any port conflicts
   in the loopback in case we will use this port number...
*/
#define PORT_FOR_UX     0x18

struct sockmod_info {
	int domain;
	int type;
	int sfd;
	int options;
	int state;
	int msgstate;
	int state_arg;
	int ux_addr;
};

#define STATE_OK_ACK    1
#define STATE_CONN_CON  2
#define STATE_ACCEPTED  3

struct sysv_udata {
	int     tidusize;       /* TIDU size          */
	int     addrsize;       /* address size       */
	int     optsize;        /* options size       */
	int     etsdusize;      /* expedited size     */
	int     servtype;       /* service type       */
	int     so_state;       /* socket states      */
	int     so_options;     /* socket options     */
};

struct sysv_bind_ux {
	unsigned short sun_family;
	char    sun_path[108];
	unsigned int  add_size;
	unsigned short dev;
	unsigned short ino;
};

extern int sys_socket (int, int, int),
	   sys_bind (int, void *, int),
	   sys_listen (int, int),
	   sys_accept (int, void *, void *),
	   sys_connect (int, void *, int),
	   sys_setsockopt (int, int, int, void *, int),
	   sys_getsockopt (int, int, int, void *, void *),
	   sys_read (unsigned int, char *, int),
	   sys_write (int, void *, int),
	   sys_shutdown (int, int),
	   sys_getsockname (int, void *, void *),
	   sys_getpeername (int, void *, void *),
	   sys_send (int, void *, int, int),
	   sys_sendto (int, void *, int, int, void *, int),
	   sys_recv (int, void *, int, int),
	   sys_recvfrom (int, void *, int, int, void *, void *);


static int sockmod_open (struct stream_info *stream_info, struct file *file,
				struct module_operations *prev_module);
static void sockmod_close (struct stream_info *stream_info, struct file *file);
static int sockmod_ioctl (struct stream_info *stream_info, int cmd,
					void *buf, int *buflen, int timeout);
static int sockmod_flush (struct stream_info *stream_info, int rw);
static int sockmod_nread (struct stream_info *stream_info, int *nbytes);
static int sockmod_getmsg (struct stream_info *stream_info,
				void *buf_ctl, int *len_ctl, void *buf_data,
				int *len_data, int nonblocks, int *flags);
static int sockmod_peekmsg (struct stream_info *stream_info,
				void *buf_ctl, int *len_ctl, void *buf_data,
				int *len_data, int *flags);
static int sockmod_read (struct stream_info *stream_info, void *buf,
						int count, int nonblocks);
static int sockmod_putmsg (struct stream_info *stream_info,
				void *buf_ctl, int len_ctl, void *buf_data,
				int len_data, int nonblocks, int flags);
static int sockmod_select (struct stream_info *stream_info, struct file *file,
					    int sel_type, select_table *wait);
static int sockmod_sendfd (struct stream_info *stream_info, struct file *file,
								int fd);
static int sockmod_recvfd (struct stream_info *stream_info, struct file *file,
					    int *uid, int *gid, char *fill);


#if 1
static struct module_operations sockmod_mod_ops = {
	"sockmod",              /*  name   */
	NULL,                   /*  next in linked list   */
	sockmod_open,           /*  called when I_PUSH   */
	sockmod_close,          /*  called when I_POP or close stream device */
	NULL,                   /*  I_FIND, is somewhat under us ?   */
	sockmod_ioctl,          /*  called for I_STR   */
	sockmod_flush,          /*  called for I_FLUSH   */
	sockmod_nread,          /*  called for I_NREAD   */
	sockmod_getmsg,         /*  get message routine   */
	sockmod_peekmsg,        /*  get message without removing...   */
	sockmod_read,           /*  read routine for *normal* read mode   */
	sockmod_putmsg,         /*  put message routine   */
	sockmod_select,         /*  select (the same semantic as a poll)   */
	NULL,                   /*  called for I_LINK   */
	NULL,                   /*  called for I_UNLINK   */
	sockmod_sendfd,         /*  called for I_SENDFD   */
	sockmod_recvfd          /*  called for I_RECVFD   */
};
#else

static int d_sockmod_open (struct stream_info *stream_info, struct file *file,
				struct module_operations *prev_module) {
	int err;

	printk ("(%s): sockmod_open: %08x, %08x, %08x : ",
			current->comm, stream_info, file, prev_module);
	err = sockmod_open (stream_info, file, prev_module);
	printk ("res = %d\n", err);

	return err;
}

static void d_sockmod_close (struct stream_info *stream_info, struct file *file) {

	printk ("(%s): sockmod_close: %08x, %08x\n",
			current->comm, stream_info, file);
	sockmod_close (stream_info, file);

	return;
}

static int d_sockmod_ioctl (struct stream_info *stream_info, int cmd,
					void *buf, int *buflen, int timeout) {
	int err;

	printk ("(%s): sockmod_ioctl: %08x, cmd=0x%08x, %08x, %d, %d : ",
		current->comm, stream_info, cmd, buf, *buflen, timeout);
	err = sockmod_ioctl (stream_info, cmd, buf, buflen, timeout);
	printk ("res = %d (retlen = %d)\n", err, *buflen);

	return err;
}

static int d_sockmod_getmsg (struct stream_info *stream_info,
				void *buf_ctl, int *len_ctl, void *buf_data,
				int *len_data, int nonblocks, int *flags) {
	int err;

	printk ("(%s): sockmod_getmsg: %08x,  %08x, %d,  %08x, %d,  %d, %d : ",
		current->comm, stream_info, buf_ctl, *len_ctl,
			buf_data, *len_data, nonblocks, *flags);
	err = sockmod_getmsg (stream_info, buf_ctl, len_ctl, buf_data, len_data,
						nonblocks, flags);
	printk ("res = %d (ctl = %d, data = %d)\n", err, *len_ctl, *len_data);

	return err;
}

static int d_sockmod_read (struct stream_info *stream_info, void *buf,
						int count, int nonblocks) {
	int err;

	printk ("(%s): sockmod_read: %08x, %08x, %d, %d : ",
			current->comm, stream_info, buf, count, nonblocks);
	err = sockmod_read (stream_info, buf, count, nonblocks);
	printk ("res = %d\n", err);

	return err;
}

static int d_sockmod_putmsg (struct stream_info *stream_info,
				void *buf_ctl, int len_ctl, void *buf_data,
				int len_data, int nonblocks, int flags) {
	int err;

	if (len_ctl >= 0)
	printk ("(%s): sockmod_putmsg: %08x,  %08x, %d,  %08x, %d,  %d, %d : ",
		current->comm, stream_info, buf_ctl, len_ctl,
				buf_data, len_data, nonblocks, flags);
	err = sockmod_putmsg (stream_info, buf_ctl, len_ctl, buf_data, len_data,
						nonblocks, flags);
	if (len_ctl >= 0)
	printk ("res = %d\n", err);

	return err;
}

static int d_sockmod_select (struct stream_info *stream_info, struct file *file,
					    int sel_type, select_table *wait) {
	int err;

	printk ("(%s): sockmod_select: %08x, %08x, %d, %08x : ",
		current->comm, stream_info, file, sel_type, wait);
	err = sockmod_select (stream_info, file, sel_type, wait);
	printk ("res = %d\n", err);

	return err;
}

static int d_sockmod_flush (struct stream_info *stream_info, int rw) {
	int err;

	printk ("(%s): sockmod_flush: %08x, %d : ",
				current->comm, stream_info, rw);
	err = sockmod_flush (stream_info, rw);
	printk ("res = %d\n", err);

	return err;
}

static int d_sockmod_nread (struct stream_info *stream_info, int *nbytes) {
	int err;

	printk ("(%s): sockmod_nread: %08x, %d : ",
				current->comm, stream_info, *nbytes);
	err = sockmod_nread (stream_info, nbytes);
	printk ("res = %d (ret_len = %d)\n", err, *nbytes);

	return err;
}

static int d_sockmod_peekmsg (struct stream_info *stream_info,
				void *buf_ctl, int *len_ctl, void *buf_data,
				int *len_data, int *flags) {
	int err;

	printk ("(%s): sockmod_peekmsg: %08x,  %08x, %d,  %08x, %d,  %d : ",
		current->comm, stream_info, buf_ctl, *len_ctl,
				buf_data, *len_data, *flags);
	err = sockmod_peekmsg (stream_info, buf_ctl, len_ctl,
						buf_data, len_data, flags);
	printk ("res = %d (ctl = %d, data = %d)\n", err, *len_ctl, *len_data);

	return err;
}

static int d_sockmod_link (struct stream_info *stream_info, struct file *file,
								int fd) {

	printk ("(%s): trying sockmod_link: %08x, %08x, %d\n",
				current->comm, stream_info, file, fd);

	return -EINVAL;
}

static int d_sockmod_unlink (struct stream_info *stream_info, struct file *file,
								int id) {

	printk ("(%s): trying sockmod_unlink: %08x, %08x, %d\n",
				current->comm, stream_info, file, id);

	return -EINVAL;
}

static int d_sockmod_sendfd (struct stream_info *stream_info,
					struct file *file, int fd) {

	printk ("(%s): trying sockmod_sendfd: %08x, %08x, %d\n",
				current->comm, stream_info, file, fd);

	return -EINVAL;
}

static int d_sockmod_recvfd (struct stream_info *stream_info,
			struct file *file, int *uid, int *gid, char *fill) {

	printk ("(%s): trying sockmod_recvfd: %08x, %08x, ...\n",
				current->comm, stream_info, file);

	return -EINVAL;
}

static int d_sockmod_find (struct stream_info *stream_info, char *name) {

	printk ("(%s): trying sockmod_find: %08x, %s\n",
				current->comm, stream_info, name);

	return 0;
}


static struct module_operations sockmod_mod_ops = {
	"sockmod",              /*  name   */
	NULL,                   /*  next in linked list   */
	d_sockmod_open,           /*  called when I_PUSH   */
	d_sockmod_close,          /*  called when I_POP or close stream device */
	d_sockmod_find,           /*  I_FIND, is somewhat under us ?   */
	d_sockmod_ioctl,          /*  called for I_STR   */
	d_sockmod_flush,          /*  called for I_FLUSH   */
	d_sockmod_nread,          /*  called for I_NREAD   */
	d_sockmod_getmsg,         /*  get message routine   */
	d_sockmod_peekmsg,        /*  get message without removing...   */
	sockmod_read,           /*  read routine for *normal* read mode   */
	d_sockmod_putmsg,         /*  put message routine   */
	sockmod_select,         /*  select (the same semantic as a poll)   */
	d_sockmod_link,         /*  called for I_LINK   */
	d_sockmod_unlink,       /*  called for I_UNLINK   */
	d_sockmod_sendfd,       /*  called for I_SENDFD   */
	d_sockmod_recvfd,       /*  called for I_RECVFD   */
};
#endif  /*  0   */


void sockmod_init (void) {

	reguster_stream_module (&sockmod_mod_ops);

	/*  no idea what`s else   */
	return;
}

struct ux_name_entry {
	struct ux_name_entry *next;
	int addr;
	char name[108];
};

static struct ux_name_entry *ux_names_base = NULL;
static int ux_names_count = 0;

#define UX_NAMES_LIMIT  128

static void insert_ux_name (char *name, int addr) {
	struct ux_name_entry *new, *tmp;

	if (!name || !name[0])  return;

	if (ux_names_count > UX_NAMES_LIMIT) {
		printk ("sockmod:  Too many ux names used: %d > %d\n",
					ux_names_count, UX_NAMES_LIMIT);
		return;
	}

	for (tmp = ux_names_base; tmp; tmp = tmp->next) {
	    if (tmp->addr == addr) {
		memset (tmp->name, 0, sizeof (tmp->name));
		strncpy (tmp->name, name, sizeof (tmp->name));

		return;
	    }
	}

	new = (struct ux_name_entry *) kmalloc (sizeof (*new), GFP_KERNEL);
	if (!new)  return;

	new->addr = addr;
	new->next = NULL;
	memset (new->name, 0, sizeof (new->name));
	strncpy (new->name, name, sizeof (new->name));

	if (!ux_names_base)
		ux_names_base = new;
	else {
	    for (tmp = ux_names_base; tmp->next; tmp = tmp->next) ;
	    tmp->next = new;
	}
	ux_names_count++;

	return;
}

static void find_ux_name (char *buf, int addr) {
	struct ux_name_entry *tmp;

	for (tmp = ux_names_base; tmp; tmp = tmp->next) {
	    if (tmp->addr == addr) {
		strncpy (buf, tmp->name, sizeof (tmp->name));

		return;
	    }
	}

	memset (buf, 0, sizeof (tmp->name));
	return;
}

static void delete_ux_name (int addr) {
	struct ux_name_entry *tmp, *last;

	for (last = NULL, tmp = ux_names_base; tmp; tmp = tmp->next) {
		if (tmp->addr == addr)  break;
		last = tmp;
	}

	if (!tmp)  return;

	if (!last)
	    ux_names_base = ux_names_base->next;
	else
	    last->next = tmp->next;

	kfree (tmp);
	ux_names_count--;

	return;
}


static int sockmod_open (struct stream_info *stream_info, struct file *file,
				    struct module_operations *prev_module) {
	int device = file->f_inode->i_rdev;
	struct sockmod_info *sockmod_info;
	int domain, type;

	if (prev_module) {

	    printk ("(%s): stream `%s' (%d,%d): "
		    "found a previous module `%s'\n",
		    current->comm, stream_info->m_ops->name,
		    MAJOR (device), MINOR (device), prev_module->name);

	    /*  try correct way to handle this case   */
	    if (prev_module->close) {
		struct module_operations *new_module = stream_info->m_ops;

		stream_info->m_ops = prev_module;
		prev_module->close (stream_info, file);

		stream_info->m_ops = new_module;
	    }
	}

	stream_info->max = 0;
	stream_info->min = 0;

	/*  do an ordinary  sys_socket  at this point   */
	/*  first, get `domain' and `type'
	   (currently only default `protocol')
	*/

	switch (MAJOR (device)) {

	    case UNIX_STREAM_MAJOR:
		domain = AF_UNIX;
		type = SOCK_STREAM;
		break;

	    case UNIX_DGRAM_MAJOR:
		domain = AF_UNIX;
		type = SOCK_DGRAM;
		break;

	    case INET_STREAM_MAJOR:
		domain = AF_INET;
		type = SOCK_STREAM;
		break;

	    case INET_DGRAM_MAJOR:
		domain = AF_INET;
		type = SOCK_DGRAM;
		break;

	    default:
		printk ("(%s): Can`t understand stream sockmod semantic "
			"by major %d\n", current->comm, MAJOR (device));
		return -ENXIO;
		break;
	}

	sockmod_info = kmalloc (sizeof (*sockmod_info), GFP_KERNEL);
	if (!sockmod_info)  return -ENOMEM;

	memset (sockmod_info, 0, sizeof (*sockmod_info));

	stream_info->module_data = sockmod_info;

	sockmod_info->domain = domain;
	sockmod_info->type = type;

	sockmod_info->sfd = sys_socket (AF_INET, type, 0);
	if (sockmod_info->sfd < 0)  return sockmod_info->sfd;

	return 0;
}

static void sockmod_close (struct stream_info *stream_info,
						struct file *file) {
	struct sockmod_info *sockmod_info = stream_info->module_data;

	if (!sockmod_info)  return;

	if (current->files)     /*  we are not on do_exit() */
		sys_close (sockmod_info->sfd);

	if (sockmod_info->ux_addr)  delete_ux_name (sockmod_info->ux_addr);

	kfree (sockmod_info);
	stream_info->module_data = NULL;

	stream_info->m_ops = NULL;

	return;
}


/*  stream module ioctl stuff   */

static int sockmod_sockopt (struct sockmod_info *sockmod_info,
						void *buf, int buflen) {
	long *lp, *op;
	int optname, err;

	lp = (long *) buf;

	if (lp[0] != 9 ||       /*  T_OPTMGMT_REQ   */
	    lp[1] + lp[2] > buflen
	)  return -EINVAL;

	op = (long *) (buf + lp[2]);

	/*  op[0] -- level,
	    op[1] -- optname,
	    op[2] -- optlen
	*/
#if 0
	printk ("{%04x %d} : ", op[1], op[3]);
#endif
#define sysv_optname    (op[1])
#define optlen          (op[2])

	if (optlen + 12 > lp[1])  return -EINVAL;

	if (op[0] != 0xffff)  return -EINVAL;   /*  only SOL_SOCKET  */

	optname = 0;

	switch (sysv_optname) {

	    case 0x0001:  optname = SO_DEBUG;  break;
	    case 0x0002:  break;  /*  SO_ACCEPTCONN  */
	    case 0x0004:  optname = SO_REUSEADDR;  break;
	    case 0x0008:  optname = SO_KEEPALIVE;  break;
	    case 0x0010:  optname = SO_DONTROUTE;  break;
	    case 0x0020:  optname = SO_BROADCAST;  break;
	    case 0x0040:  break;  /*  SO_USELOOPBACK  */
	    case 0x0080:  optname = SO_LINGER;  break;
	    case 0x0100:  optname = SO_OOBINLINE;  break;
	    case 0x0200:  break;  /*  SO_ORDREL  */
	    case 0x0400:  break;  /*  SO_IMASOCKET  */

	    case 0x1001:  optname = SO_SNDBUF;  break;
	    case 0x1002:  optname = SO_RCVBUF;  break;
	    case 0x1003:  break;  /*  SO_SNDLOWAT  */
	    case 0x1004:  break;  /*  SO_RCVLOWAT  */
	    case 0x1005:  break;  /*  SO_SNDTIMEO  */
	    case 0x1006:  break;  /*  SO_RCVTIMEO  */
	    case 0x1007:  optname = SO_ERROR;  break;
	    case 0x1008:  optname = SO_TYPE;  break;
	    case 0x1009:  break;  /*  SO_PROTOTYPE  */

	    default:  return -EINVAL;  break;
	}

	if (lp[3] == 0x4) {     /*  setsockopt   */

	    if (optname) {
		set_fs (KERNEL_DS);
		err = sys_setsockopt (sockmod_info->sfd, SOL_SOCKET,
						    optname, &op[3], optlen);
		set_fs (USER_DS);
		if (err)  return err;
	    }

	    if (!(sysv_optname & 0x1000))
		    sockmod_info->options |= sysv_optname;
	}
	else if (lp[3] == 0x8) {        /*  getsockopt   */

	    if (optname) {
		set_fs (KERNEL_DS);
		err = sys_getsockopt (sockmod_info->sfd, SOL_SOCKET,
						    optname, &op[3], &optlen);
		if (err)  return err;
		set_fs (USER_DS);

	    } else
		memset (&op[3], 0, optlen);

	    if (!(sysv_optname & 0x1000))
		    sockmod_info->options &= ~sysv_optname;
	}
	else  return -EINVAL;

#undef sysv_optname
#undef optlen

	lp[0] = 22;     /*  T_OPTMGMT_ACK   */

	return 0;
}



static int sockmod_ioctl (struct stream_info *stream_info, int cmd,
					void *buf, int *buflen, int timeout) {
	struct sockmod_info *sockmod_info = stream_info->module_data;
	struct sysv_udata *udata;
	char data[256];
	long *lp;
	struct sockaddr_in *sin, inet_addr;
	struct sysv_bind_ux *bux;
	int err, how;

	if (!sockmod_info)  return -EINVAL;     /*  yet not initialized   */

	if (*buflen > sizeof (data))  *buflen = sizeof (data);

	switch (cmd) {

	    case 0x4965:        /*  SI_GETUDATA   */
		udata = (struct sysv_udata *) data;

		udata->tidusize = 2048;         /*  intuition...  */
		udata->addrsize = (sockmod_info->domain == AF_INET)
					? sizeof (struct sockaddr_in)
					: sizeof (struct sysv_bind_ux);
		udata->optsize = 0;
		udata->etsdusize = 0;
		udata->servtype = sockmod_info->type == SOCK_DGRAM
					? 3     /*  T_CLTS   */
					: 1;    /*  T_COTS   */
		udata->so_state = sockmod_info->state;
		udata->so_options = sockmod_info->options;

		if (*buflen > sizeof (*udata))  *buflen = sizeof (*udata);

		memcpy_tofs (buf, udata, *buflen);

		return 0;
		break;

	    case 0x548d:        /*  TI_OPTMGMT   */
		memcpy_fromfs (data, buf, *buflen);

		err = sockmod_sockopt (sockmod_info, data, *buflen);
		if (err < 0)  return err;

		memcpy_tofs (buf, data, *buflen);

		return err;
		break;

	    case 0x548e:        /*  TI_BIND   */
		memcpy_fromfs (data, buf, *buflen);

		lp = (long *) data;

		if (lp[0] != 6 ||       /*  T_BIND_REQ   */
		    lp[1] + lp[2] > *buflen
		)  return -EINVAL;

		sin = (struct sockaddr_in *) (data + lp[2]);
		bux = (struct sysv_bind_ux *)  sin;

		/*  We ignore `null-bind' requests, because
		  they are not needed under the Linux`s sockets.
		*/

		if (lp[1] != 0 &&
		    (bux->sun_family != AF_UNIX || bux->ino != 0)
		) {
		    /*  real bind, handle it   */

		    if (sockmod_info->domain != sin->sin_family)
			    return -EINVAL;

		    if (strnlen (bux->sun_path, sizeof (bux->sun_path)) ==
							sizeof (bux->sun_path)
		    )  return -ENAMETOOLONG;

		    if (sin->sin_family == AF_UNIX) { /* convert to loopback */
			int addr = ((bux->dev & 0xff) << 16) |
				   (bux->ino & 0xffff);

			inet_addr.sin_family = AF_INET;
			inet_addr.sin_port = PORT_FOR_UX;
			inet_addr.sin_addr.s_addr = 0x7f000000 | addr;

			sin = &inet_addr;

			insert_ux_name (bux->sun_path, addr);
			sockmod_info->ux_addr = addr;
		    }

		    set_fs (KERNEL_DS);
		    err = sys_bind (sockmod_info->sfd, sin, sizeof (*sin));
		    set_fs (USER_DS);
		    if (err) {
			if (sockmod_info->ux_addr)
				delete_ux_name (sockmod_info->ux_addr);
			return err;
		    }
		}

		lp[0] = 17;     /*  T_BIND_ACK   */

		sockmod_info->state |= 0x80;    /*  SS_ISBOUND   */

		put_user (lp[0], (long *) buf);     /*  it`s enough   */

		return 0;
		break;

	    case 0x548f:        /*  TI_UNBIND   */
		if (get_user ((int *) buf) != 7)    /*  T_UNBIND_REQ   */
			return -EINVAL;

		/*  nothing real   */

		sockmod_info->state &= ~0x80;   /*  SS_ISBOUND   */

		return 0;
		break;

	    case 0x4967:        /*  SI_LISTEN   */
		if (sockmod_info->type == SOCK_DGRAM)  return -EINVAL;
						    /*  it want this   */
		memcpy_fromfs (data, buf, 4 * sizeof (int));

		lp = (long *) data;

		if (lp[0] != 6)  return -EINVAL;        /*  T_BIND_REQ   */

		err = sys_listen (sockmod_info->sfd, lp[3]);
		if (err)  return err;

		sockmod_info->options |= 0x2;   /*  SO_ACCEPTCONN   */

		return 0;
		break;

	    case 0x4966:        /*  SI_SHUTDOWN   */
		how = get_user ((int *) buf);

		err = sys_shutdown (sockmod_info->sfd, how);
		if (err < 0)  return err;

		how++;
		if (how & 0x1)  sockmod_info->state |= 0x20;
						/*  SS_CANTRCVMORE   */
		if (how & 0x2)  sockmod_info->state |= 0x10;
						/*  SS_CANTSENDMORE   */
		return 0;
		break;

	    /*  coming here from a kludge...  */

	    case 0x5490:        /*  TI_GETMYNAME   */
		err = verify_area (VERIFY_WRITE, buf, 3 * sizeof (int));
		if (err)  return err;

		memcpy_fromfs (data, buf, 3 * sizeof (int));

		lp = (long *) data;

		/*  lp[0] -- maxlen,
		    lp[1] -- returned len,
		    lp[2] -- bufptr
		*/

		lp[1] = sizeof (struct sockaddr_in);
		set_fs (KERNEL_DS);
		err = sys_getsockname (sockmod_info->sfd, &lp[3], &lp[1]);
		set_fs (USER_DS);
		if (err < 0)  return err;

		if (sockmod_info->domain == AF_UNIX) {
		    /*  convert from loopback   */
		    sin = (struct sockaddr_in *) &lp[3];

		    bux = (struct sysv_bind_ux *) sin;
		    bux->sun_family = AF_UNIX;
		    bux->dev = (sin->sin_addr.s_addr >> 16) & 0xff;
		    bux->ino = sin->sin_addr.s_addr & 0xffff;
		    bux->add_size = 4;
		    /*  only here!  (common sin/bux memory)   */
		    find_ux_name (bux->sun_path, sin->sin_addr.s_addr);

		    lp[1] = sizeof (*bux) - 8;
		}

		if (lp[1] > lp[0])  return -EFAULT;
		err = verify_area (VERIFY_WRITE, (void *) lp[2], lp[1]);
		if (err)  return err;

		memcpy_tofs ((void *) lp[2], &lp[3], lp[1]);
		memcpy_tofs (buf, data, 3 * sizeof (int));

		return 0;
		break;

	    case 0x5491:        /*  TI_GETPEERNAME   */
		err = verify_area (VERIFY_WRITE, buf, 3 * sizeof (int));
		if (err)  return err;

		memcpy_fromfs (data, buf, 3 * sizeof (int));

		lp = (long *) data;

		/*  lp[0] -- maxlen,
		    lp[1] -- returned len,
		    lp[2] -- bufptr
		*/

		lp[1] = sizeof (struct sockaddr_in);
		set_fs (KERNEL_DS);
		err = sys_getpeername (sockmod_info->sfd, &lp[3], &lp[1]);
		set_fs (USER_DS);
		if (err < 0)  return err;

		if (sockmod_info->domain == AF_UNIX) {
		    /*  convert from loopback   */
		    sin = (struct sockaddr_in *) &lp[3];

		    bux = (struct sysv_bind_ux *) sin;
		    bux->sun_family = AF_UNIX;
		    bux->dev = (sin->sin_addr.s_addr >> 16) & 0xff;
		    bux->ino = sin->sin_addr.s_addr & 0xffff;
		    bux->add_size = 4;
		    /*  only here!  (common sin/bux memory)   */
		    find_ux_name (bux->sun_path, sin->sin_addr.s_addr);

		    lp[1] = sizeof (*bux) - 8;
		}

		if (lp[1] > lp[0])  return -EFAULT;
		err = verify_area (VERIFY_WRITE, (void *) lp[2], lp[1]);
		if (err)  return err;

		memcpy_tofs ((void *) lp[2], &lp[3], lp[1]);
		memcpy_tofs (buf, data, 3 * sizeof (int));

		return 0;
		break;


	    default:
		return -EINVAL;
		break;
	}

	return 0;       /*  not  reached   */
}


static int sockmod_putmsg (struct stream_info *stream_info,
				void *buf_ctl, int len_ctl, void *buf_data,
				int len_data, int nonblocks, int flags) {
	struct sockmod_info *sockmod_info = stream_info->module_data;
	char data[256];
	struct sockaddr_in *sin, inet_addr;
	struct sysv_bind_ux *bux;
	long *lp;
	struct file *new_file;
	int new_sfd, err;
	struct sockmod_info *new_sock;
	char *tmp_buf;

	if (len_ctl >= 0) {     /*  there is a ctl message...   */

	    if (len_ctl > sizeof (data))  return -ERANGE;

	    if (len_ctl < 4 || !buf_ctl)  return -EBADMSG;

	    memcpy_fromfs (data, buf_ctl, len_ctl);

	    /*  get a request type from first long word in the buf_ctl  */
	    lp = (long *) data;

#if 0
	    printk ("{%d} : ", lp[0]);
#endif
	    switch (lp[0]) {

		case 0:     /*  T_CONN_REQ   */
		    if (lp[1] + lp[2] > len_ctl)  return -EINVAL;

		    if (lp[1] == 0)  return -EINVAL;

		    sin = (struct sockaddr_in *) (data + lp[2]);
		    bux = (struct sysv_bind_ux *) sin;

		    if (sockmod_info->domain != sin->sin_family)
			    return -EINVAL;

		    if (sin->sin_family == AF_UNIX) { /* convert to loopback */

			inet_addr.sin_family = AF_INET;
			inet_addr.sin_port = PORT_FOR_UX;
			inet_addr.sin_addr.s_addr =
			    0x7f000000 | ((bux->dev & 0xff) << 16) | bux->ino;

			sin = &inet_addr;
		    }

		    set_fs (KERNEL_DS);
		    err = sys_connect (sockmod_info->sfd, sin, sizeof (*sin));
		    set_fs (USER_DS);
		    if (err)  return err;

		    sockmod_info->msgstate = STATE_OK_ACK;
		    sockmod_info->state_arg = 0;   /*  T_CONN_REQ   */

		    sockmod_info->state |= 0x2;     /*  SS_ISCONNECTED   */

		    return 0;
		    break;

		case 1:     /*  T_CONN_RES   */
		    if (len_ctl < 20)  return -EINVAL;

		    if (sockmod_info->msgstate != STATE_ACCEPTED ||
			sockmod_info->state_arg != lp[4]
		    )  return -EBADMSG;

		    new_file = (struct file *) lp[1];
		    new_sfd = lp[4];    /*  or `sockmod_info->state_arg' ??  */

		    if (!new_file->f_inode ||
			!new_file->f_inode->u.generic_ip
		    )  return -ENOTSOCK;

		    new_sock = ((struct stream_info *)
				new_file->f_inode->u.generic_ip)->module_data;
		    if (!new_sock)  return -ENOTSOCK;

		    sys_close (new_sock->sfd);
		    new_sock->sfd = new_sfd;

		    new_sock->state |= 0x2;     /*  SS_ISCONNECTED   */
		    new_sock->options = sockmod_info->options;

		    sockmod_info->msgstate = STATE_OK_ACK;
		    sockmod_info->state_arg = 1;   /*  T_CONN_RES   */

		    return 0;
		    break;

		case 4:     /*  T_EXDATA_REQ   */
		    if (len_ctl < 8)  return -EINVAL;

		    if (len_data <= 0)  return 0;

		    tmp_buf = (char *) kmalloc (len_data, GFP_KERNEL);
		    if (!tmp_buf)  return -ENOMEM;

		    memcpy_fromfs (tmp_buf, buf_data, len_data);

		    set_fs (KERNEL_DS);
		    err = sys_send (sockmod_info->sfd,
					tmp_buf, len_data, MSG_OOB);
		    set_fs (USER_DS);

		    kfree (tmp_buf);

		    if (err < 0)  return err;
		    else  return 0;

		    break;

		case 8:     /*  T_UNITDATA_REQ   */
		    if (len_ctl < 20)  return -EINVAL;

		    if (len_data <= 0)  return 0;

		    sin = (struct sockaddr_in *) (data + lp[2]);
		    if (sockmod_info->domain != sin->sin_family)
			    return -EINVAL;

		    if (sockmod_info->domain == AF_UNIX) {

			inet_addr.sin_family = AF_INET;
			inet_addr.sin_port = PORT_FOR_UX;
			inet_addr.sin_addr.s_addr =
			    0x7f000000 | (lp[5] & 0x00ffffff);

			sin = &inet_addr;
		    }

		    tmp_buf = (char *) kmalloc (len_data, GFP_KERNEL);
		    if (!tmp_buf)  return -ENOMEM;

		    memcpy_fromfs (tmp_buf, buf_data, len_data);

		    set_fs (KERNEL_DS);
		    err = sys_sendto (sockmod_info->sfd, tmp_buf, len_data, 0,
							    sin, sizeof (*sin));
		    set_fs (USER_DS);

		    kfree (tmp_buf);

		    if (err < 0)  return err;
		    else  return 0;

		    break;

		default:
		    return -EBADMSG;
		    break;
	    }
	}

	/*  or this is a data write requests   */

	err = sys_write (sockmod_info->sfd, buf_data, len_data);

	return  err < 0 ? err : 0;
}


static int sockmod_getmsg (struct stream_info *stream_info,
				void *buf_ctl, int *len_ctl, void *buf_data,
				int *len_data, int nonblocks, int *flags) {
	struct sockmod_info *sockmod_info = stream_info->module_data;
	char data[256];
	struct sockaddr_in *sin;
	struct sysv_bind_ux *bux;
	long *lp;
	int err;
	char *tmp_buf;

	if (*len_ctl >= 0) {

	    if (*len_ctl < 4 || !buf_ctl)  return -EBADMSG;

	    if (sockmod_info->msgstate == STATE_OK_ACK) {
		lp = (long *) buf_ctl;

		put_user (19, lp);      /*  T_OK_ACK   */
		lp++;
		put_user (sockmod_info->state_arg, lp);

		*len_ctl = 8;
		*flags = 0x1;

		sockmod_info->msgstate = 0;

		if (sockmod_info->state_arg == 0) {     /*  was T_CONN_REQ   */
		    if (sockmod_info->type != SOCK_DGRAM)
			sockmod_info->msgstate = STATE_CONN_CON;
		    else
			sockmod_info->state |= 0x2;   /*  SS_ISCONNECTED  */
		}

		return 0;
	    }

	    if (sockmod_info->msgstate == STATE_CONN_CON) {
		lp = (long *) data;

		lp[0] = 12;     /*  T_CONN_CON   */
		lp[1] = 16;
		lp[2] = 20;
		lp[3] = lp[4] = 0;
		memset (&lp[5], 0, 16);     /*  ????????   */

		*len_ctl = 36;      /*  true only for AF_INET ???   */
		memcpy_tofs (buf_ctl, data, *len_ctl);

		sockmod_info->msgstate = 0;
		sockmod_info->state |= 0x2;     /*  SS_ISCONNECTED   */

		return 0;
	    }

	    if (sockmod_info->msgstate == 0 &&
		*len_ctl > 24 &&
		*len_data < 0
	    ) {
		/*  assume it want to accept...   */

		lp = (long *) data;

		lp[0] = 11;     /*  T_CONN_IND   */
		lp[1] = 16;     /*  sizeof (struct sockaddr_in)   */
		lp[2] = 24;     /*  offset, where address is   */
		lp[3] = lp[4] = lp[5] = 0;

		set_fs (KERNEL_DS);
		err = sys_accept (sockmod_info->sfd, data + lp[2], &lp[1]);
		set_fs (USER_DS);
		if (err < 0)  return err;

		sockmod_info->msgstate = STATE_ACCEPTED;
		sockmod_info->state_arg = err;      /*  new socket fd   */

		lp[5] = err;    /*  sequence number ???   */

		if (sockmod_info->domain == AF_UNIX) {
		    /*  convert from loopback   */
		    sin = (struct sockaddr_in *) (data + lp[2]);

		    bux = (struct sysv_bind_ux *) sin;
		    bux->sun_family = AF_UNIX;
		    bux->dev = (sin->sin_addr.s_addr >> 16) & 0xff;
		    bux->ino = sin->sin_addr.s_addr & 0xffff;
		    bux->add_size = 4;
		    /*  only here!  (common sin/bux memory)   */
		    find_ux_name (bux->sun_path, sin->sin_addr.s_addr);

		    lp[1] = 2 + strlen (bux->sun_path) + 1;
		}

		*len_ctl = MIN (*len_ctl, 24 + lp[1]);
		memcpy_tofs (buf_ctl, data, *len_ctl);

		return 0;
	    }

	    if (sockmod_info->msgstate == 0 &&
		*len_data >= 0
	    ) {     /*  assume it want recvmsg...   */

		tmp_buf = (char *) kmalloc (*len_data, GFP_KERNEL);
		if (!tmp_buf)  return -ENOMEM;

		if (sockmod_info->type != SOCK_DGRAM) {

		    set_fs (KERNEL_DS);
		    err = sys_recv (sockmod_info->sfd, tmp_buf, *len_data, 0);
		    set_fs (USER_DS);
		    if (err < 0)  { kfree (tmp_buf);  return err; }

		    *len_ctl = -1;      /*  is it correct ???  */
		    *len_data = err;
		    memcpy_tofs (buf_data, tmp_buf, *len_data);

		} else {
		    lp = (long *) data;

		    lp[0] = 20;         /*  T_UNITDATA_IND   */
		    lp[1] = 16;
		    lp[2] = 20;
		    lp[3] = lp[4] = 0;

		    set_fs (KERNEL_DS);
		    err = sys_recvfrom (sockmod_info->sfd, tmp_buf, *len_data,
				    0, (struct sockaddr_in *) &lp[5], &lp[1]);
		    set_fs (USER_DS);
		    if (err < 0)  { kfree (tmp_buf);  return err; }

		    *len_data = err;
		    memcpy_tofs (buf_data, tmp_buf, *len_data);

		    if (sockmod_info->domain == AF_UNIX) {
			/*  convert from loopback   */
			sin = (struct sockaddr_in *) &lp[5];

			bux = (struct sysv_bind_ux *) sin;
			bux->sun_family = AF_UNIX;
			bux->dev = (sin->sin_addr.s_addr >> 16) & 0xff;
			bux->ino = sin->sin_addr.s_addr & 0xffff;
			bux->add_size = 4;
			/*  only here!  (common sin/bux memory)   */
			find_ux_name (bux->sun_path, sin->sin_addr.s_addr);

			lp[1] = 2 + strlen (bux->sun_path) + 1;
		    }

		    *len_ctl = MIN (*len_ctl, 20 + lp[1]);
		    memcpy_tofs (buf_ctl, data, *len_ctl);
		}

		kfree (tmp_buf);

		return 0;
	    }

	    return -EINVAL;
	}

	/*  or this is a read requests   */

	err = sys_read (sockmod_info->sfd, buf_data, *len_data);

	if (err < 0)  return err;

	*len_data = err;

	return 0;
}


static int sockmod_flush (struct stream_info *stream_info, int rw) {

	return -ENOSYS;
}

static int sockmod_nread (struct stream_info *stream_info, int *nbytes) {
	struct sockmod_info *sockmod_info = stream_info->module_data;
	struct file *sock_file;
	int err;

	if (!sockmod_info)  return -ENOTSOCK;

	sock_file = current->files->fd[sockmod_info->sfd];

	if (!sock_file->f_op || !sock_file->f_op->ioctl)  return -EINVAL;

	set_fs (KERNEL_DS);
	err = sock_file->f_op->ioctl (sock_file->f_inode, sock_file,
					0x541b /* FIONREAD */, (int) nbytes);
	set_fs (USER_DS);

	return err;
}

static int sockmod_peekmsg (struct stream_info *stream_info,
				void *buf_ctl, int *len_ctl, void *buf_data,
				int *len_data, int *flags) {

	return -ENOSYS;
}

static int sockmod_sendfd (struct stream_info *stream_info, struct file *file,
								int fd) {

	return -ENOSYS;
}

static int sockmod_recvfd (struct stream_info *stream_info, struct file *file,
					    int *uid, int *gid, char *fill) {

	return -ENOSYS;
}



static int sockmod_read (struct stream_info *stream_info, void *buf,
						int count, int nonblocks) {
	struct sockmod_info *sockmod_info = stream_info->module_data;
	struct file *sock_file;

	if (!sockmod_info)  return -ENOTSOCK;

	sock_file = current->files->fd[sockmod_info->sfd];

	if (!sock_file->f_op || !sock_file->f_op->read)  return -EINVAL;

	return  sock_file->f_op->read (sock_file->f_inode, sock_file,
								buf, count);
}


static int sockmod_select (struct stream_info *stream_info, struct file *file,
					    int sel_type, select_table *wait) {
	struct sockmod_info *sockmod_info = stream_info->module_data;
	struct file *sock_file;

	if (!sockmod_info)  return -ENOTSOCK;

	sock_file = current->files->fd[sockmod_info->sfd];

	if (!sock_file->f_op || !sock_file->f_op->select)  return 1;

	return  sock_file->f_op->select (sock_file->f_inode, sock_file,
							    sel_type, wait);
}


