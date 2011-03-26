/*
 * besta/sockm.c -- Emulation of System V`s `/dev/so/...' driver.
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
#include <linux/sched.h>
#include <linux/errno.h>
#include <linux/termios.h>
#include <linux/interrupt.h>
#include <linux/linkage.h>
#include <linux/kernel_stat.h>
#include <linux/fcntl.h>

#include <asm/system.h>
#include <asm/pgtable.h>
#include <asm/traps.h>
#include <asm/irq.h>


static int sockm_open (struct inode *inode, struct file *filp);
static void sockm_release (struct inode *inode, struct file *filp);
static int sockm_read (struct inode *inode, struct file *filp,
						char *buf, int count);
static int sockm_write (struct inode *inode, struct file *filp,
					    const char * buf, int count);
static int sockm_select (struct inode *inode, struct file *filp,
					int sel_type, select_table *wait);
static int sockm_ioctl (struct inode *inode, struct file *filp,
					unsigned int cmd, unsigned long arg);

static struct file_operations sockm_fops = {
	NULL,
	sockm_read,
	sockm_write,
	NULL,
	sockm_select,
	sockm_ioctl,
	NULL,
	sockm_open,
	sockm_release,
	NULL,
	NULL,
	NULL,
	NULL,
};

#if 0
static int sockm_debug_open (struct inode *inode, struct file *file) {
	int retval;

	printk ("%s: sockm_open: dev=0x%04x, flags=0x%x,",
		current->comm, inode->i_rdev, file->f_flags);
	retval = sockm_open (inode, file);
	printk (" ret=%d\n", retval);

	return retval;
}

static void sockm_debug_release (struct inode *inode, struct file *filp) {

	printk ("%s: sockm_release: dev=0x%04x\n",
		current->comm, inode->i_rdev);
	sockm_release (inode, filp);
	return;
}

static int sockm_debug_read (struct inode *inode, struct file *file,
						char *buf, int count) {
	int retval;

	printk ("%s: sockm_read: dev=0x%04x, buf=%p, count=%d,",
		current->comm, inode->i_rdev, buf, count);
	retval = sockm_read (inode, file, buf, count);
	printk (" ret=%d\n", retval);

	return retval;
}

static int sockm_debug_write (struct inode *inode, struct file *file,
					    const char * buf, int count) {
	int retval;

	printk ("%s: sockm_write: dev=0x%04x, buf=%p, count=%d,",
		current->comm, inode->i_rdev, buf, count);
	retval = sockm_write (inode, file, buf, count);
	printk (" ret=%d\n", retval);

	return retval;
}

static int sockm_debug_ioctl (struct inode *inode, struct file *filp,
					unsigned int cmd, unsigned long arg) {
	int retval;

	printk ("%s: sockm_ioctl: dev=0x%04x, cmd=0x%x, arg=0x%lx,",
		current->comm, inode->i_rdev, cmd, arg);
	retval = sockm_ioctl (inode, filp, cmd, arg);
	printk (" ret=%d\n", retval);

	return retval;
}

static struct file_operations sockm_debug_fops = {
	NULL,
	sockm_debug_read,
	sockm_debug_write,
	NULL,
	sockm_select,
	sockm_debug_ioctl,
	NULL,
	sockm_debug_open,
	sockm_debug_release,
	NULL,
	NULL,
	NULL,
	NULL,
};

#endif




char sockm_busy[256] = { 0, };
char sockm_sock[256];

#define SOCKM_FREE      0
#define SOCKM_PREALLOC  1
#define SOCKM_ALLOC     2
#define SOCKM_REQUEST   3
#define SOCKM_CONN      4
#define SOCKM_DISCONN   5
#define SOCKM_LISTEN    6

struct svr3_s_select {          /* passed to the select ioctl   */
	int     rfd;
	int     wfd;
	int     tim;
	int     rrfd;
	int     rwfd;
	int     rcnt;
};

struct select_buffer {
	int nd;
	fd_set *inp;
	fd_set *outp;
	fd_set *exp;
	struct timeval *tvp;
};

extern int old_select (void *);

struct sockm {
	int state;
	int num;
	int peer;
	int reqmax;
	int reqcnt;
	int head;
	int tail;
	int cnt;
	char *data;
	struct wait_queue *wait;
	struct sockm *connq;
	char name[64];
};

struct sockm *sockm_base[256] = { NULL, };

void sockm_init (const char *name, int major) {

	if (register_chrdev (major, name, &sockm_fops))
	    printk ("Unable to get major %d\n", major);

	return;
}

static int sockm_open (struct inode *inode, struct file *filp) {
	int minor = MINOR (inode->i_rdev);
	struct sockm *sp;
	unsigned short flags;

	if (minor == 255)  return 0;    /*  master channel   */

	sp = sockm_base[minor];
	if (!sp)  return -EINVAL;       /*  svr3 behavior   */

	save_flags (flags);
	cli();

	if (sp->state == SOCKM_PREALLOC)  sp->state = SOCKM_ALLOC;
	else if (sp->state == SOCKM_REQUEST)  sp->state = SOCKM_CONN;

	restore_flags (flags);

	if (sp->state != SOCKM_ALLOC && sp->state != SOCKM_CONN)
							return -EINVAL;
	return 0;
}

static void sockm_release (struct inode *inode, struct file *filp) {
	int minor = MINOR (inode->i_rdev);
	struct sockm *sp = sockm_base[minor];
	unsigned short flags;

	if (minor == 255)  return;      /*  master channel   */

	save_flags (flags);
	cli();

	if (sp->state == SOCKM_LISTEN) {

	    sp->state = SOCKM_DISCONN;

	    while (sp->connq) {
		struct sockm *req = sp->connq;

		sp->connq = req->connq;

		if (req->state == SOCKM_CONN ||
		    req->state == SOCKM_REQUEST
		) {
		    sockm_base[req->peer]->state = SOCKM_DISCONN;

		    wake_up_interruptible (&sockm_base[req->peer]->wait);

		    sockm_base[req->peer]->peer = 0;
		}

		if (req->data)  free_page ((unsigned long) req->data);

		if (req->state == SOCKM_REQUEST ||
		    req->state == SOCKM_PREALLOC
		) {     /*  not opened   */

		    sockm_base[req->num] = NULL;
		    kfree (req);

		} else {

		    req->cnt = 0;
		    req->head = 0;
		    req->tail = 0;
		    req->state = SOCKM_DISCONN;
		}
	    }
	}

#if 0
	if ((sp->state == SOCKM_CONN ||
	     sp->state == SOCKM_DISCONN) && sp->connq != NULL
	) {
	    if (sp->connq->state == SOCKM_LISTEN)
				    sp->connq->reqcnt--; /* ???  */
	}
#endif

	if (sp->state == SOCKM_CONN) {

	    sp->state = SOCKM_DISCONN;

	    sockm_base[sp->peer]->state = SOCKM_DISCONN;
	    wake_up_interruptible (&sockm_base[sp->peer]->wait);
	    sockm_base[sp->peer]->peer = 0;
	}

	if (sp->data)  free_page ((unsigned long) sp->data);

	sockm_base[sp->num] = NULL;
	kfree (sp);

	restore_flags (flags);

	return;
}

static int sockm_read (struct inode *inode, struct file *filp,
						char *buf, int count) {
	int minor = MINOR (inode->i_rdev);
	struct sockm *sp = sockm_base[minor];
	int total = 0;
	int err;
	unsigned short flags;

	if (minor == 255)  return -EINVAL;  /*  master channel   */

	err = verify_area (VERIFY_WRITE, buf, count);
	if (err)  return err;

restart:
	if (sp->state != SOCKM_CONN &&
	    sp->state != SOCKM_DISCONN &&
	    sp->state != SOCKM_REQUEST
	)  return -EPIPE;   /*  svr3 behavior  */

	while (count && sp->cnt) {
	    int n = sp->head + sp->cnt > PAGE_SIZE ? PAGE_SIZE - sp->head
						   : sp->cnt;

	    if (n > count)  n = count;

	    memcpy_tofs (buf, &sp->data[sp->head], n);

	    count -= n;
	    buf += n;
	    total += n;
	    sp->cnt -= n;
	    sp->head += n;
	    sp->head &= (PAGE_SIZE - 1);
	}

	if (total) {
	    if (sp->state == SOCKM_CONN || sp->state == SOCKM_REQUEST)
		wake_up_interruptible (&sockm_base[sp->peer]->wait);
	    inode->i_atime = CURRENT_TIME;
	    return total;
	}

	if (sp->state == SOCKM_DISCONN)  return total;

	if (filp->f_flags & O_NONBLOCK)  return -EDEADLK;  /* svr3 behavior  */

	save_flags (flags);
	cli();

	interruptible_sleep_on (&sp->wait);

	restore_flags (flags);

	if (current->signal & ~current->blocked)  return -ERESTARTSYS;

	goto restart;

}

static int sockm_write (struct inode *inode, struct file *filp,
					    const char * buf, int count) {
	int minor = MINOR (inode->i_rdev);
	struct sockm *sp = sockm_base[minor];
	struct sockm *peer = sockm_base[sp->peer];
	int total = 0;
	int err;
	unsigned short flags;

	if (minor == 255)  return -EINVAL;  /*  master channel   */

	err = verify_area (VERIFY_READ, buf, count);
	if (err)  return err;

	while (count) {
	    int n = PAGE_SIZE - peer->cnt;  /*  free area   */


	    if (sp->state != SOCKM_CONN &&
		sp->state != SOCKM_REQUEST
	    )  return -EPIPE;   /*  svr3 behavior  */

	    if (peer->cnt == PAGE_SIZE) {

		if (filp->f_flags & O_NONBLOCK) {
		    if (total) {
			inode->i_mtime = CURRENT_TIME;
			return total;
		    } else
			return -EDEADLK;   /*  svr3 behavior  */
		}

		save_flags (flags);
		cli();

		interruptible_sleep_on (&sp->wait);

		restore_flags (flags);

		if (current->signal & ~current->blocked)  return -ERESTARTSYS;

		continue;
	    }

	    if (peer->tail + n > PAGE_SIZE)  n = PAGE_SIZE - peer->tail;
	    if (n > count)  n = count;

	    if (!peer->data) {
		peer->data = (char *) get_free_page (GFP_KERNEL);
		if (!peer->data)  return -ENOMEM;
	    }

	    memcpy_fromfs (&peer->data[peer->tail], buf, n);

	    count -= n;
	    buf += n;
	    total += n;
	    peer->tail += n;
	    peer->tail &= (PAGE_SIZE - 1);
	    peer->cnt += n;
	}

	if (total) {
	    wake_up_interruptible (&peer->wait);
	    inode->i_mtime = CURRENT_TIME;
	}

	return total;
}

static int sockm_ioctl (struct inode *inode, struct file *filp,
					unsigned int cmd, unsigned long arg) {
	int minor = MINOR (inode->i_rdev);
	struct sockm *sp = sockm_base[minor];
	int err, i;
	unsigned short flags;

	if (minor == 255)       /*  master channel   */

	    switch (cmd) {
		struct svr3_s_select ss;
		struct select_buffer selbuf;
		struct timeval tv;

		case 0x5301:    /*  SVSOCKET   */
		    err = verify_area (VERIFY_WRITE, (void *) arg,
							    sizeof (int));
		    if (err)  return err;

		    save_flags (flags);
		    cli();

		    for (i = 0; i < 255 && sockm_base[i]; i++) ;
		    if (i < 255) {
			sockm_base[i] = kmalloc (sizeof (struct sockm),
								GFP_KERNEL);
			if (sockm_base[i]) {
			    memset (sockm_base[i], 0, sizeof (struct sockm));
			    sockm_base[i]->state = SOCKM_PREALLOC;
			    sockm_base[i]->num = i;
			}
		    }

		    restore_flags (flags);

		    if (i >= 255)  return -EBUSY;
		    if (sockm_base[i] == NULL)  return -ENOMEM;

		    put_fs_long (i, (int *) arg);
		    break;

		case 0x5302:    /*  SVSELECT   */
		    /*  write checking  "includes" read too   */
		    err = verify_area (VERIFY_WRITE, (void *) arg,
							    sizeof (ss));
		    if (err)  return err;
		    memcpy_fromfs (&ss, (void *) arg, sizeof (ss));

		    ss.rrfd = ss.rfd;
		    ss.rwfd = ss.wfd;
		    ss.rcnt = 0;

		    selbuf.nd = NR_OPEN;
		    /*  Be careful. We hope fd_set actual points to longword */
		    selbuf.inp = (fd_set *) &ss.rrfd;
		    selbuf.outp = (fd_set *) &ss.rwfd;
		    selbuf.exp = NULL;
		    if (ss.tim != 0) {
			tv.tv_sec = ss.tim / HZ;
			tv.tv_usec = ((ss.tim % HZ) * 1000000) / HZ;
			selbuf.tvp = &tv;
		    } else
			selbuf.tvp = NULL;

		    set_fs (KERNEL_DS);
		    err = old_select (&selbuf);
		    set_fs (USER_DS);
		    if (err < 0)  return err;

		    ss.rcnt = err;
		    memcpy_tofs ((void *) arg, &ss, sizeof (ss));

		    break;

		case 0x537d:    /*  SGETUCBTIME   */
		    err = verify_area (VERIFY_WRITE, (void *) arg, sizeof (tv));
		    if (err)  return err;

		    tv.tv_sec = xtime.tv_sec;
		    tv.tv_usec = xtime.tv_usec;

		    memcpy_tofs ((void *) arg, &tv, sizeof (tv));

		    break;

		default:
		    return -EINVAL;
		    break;
	    }

	else                    /*  socket channel   */
	    switch (cmd) {
		struct sockm *req, *serv;
		char name[64 + 1];

		case 0x5379:    /*  SOBIND   */
		    err = verify_area (VERIFY_READ, (void *) arg, 64);
		    if (err)  return err;

		    if (sp->state == SOCKM_CONN ||
			sp->state == SOCKM_DISCONN ||
			sp->state == SOCKM_REQUEST
		    )  return -EMLINK;      /*  svr3 behavior  */

		    memcpy_fromfs (sp->name, (void *) arg, 64);

		    break;

		case 0x537a:    /*  SOLISTEN   */
		    err = verify_area (VERIFY_READ, (void *) arg,
							    sizeof (int));
		    if (err)  return err;

		    if (sp->state == SOCKM_CONN ||
			sp->state == SOCKM_DISCONN ||
			sp->state == SOCKM_REQUEST
		    )  return -EMLINK;      /*  svr3 behavior  */

		    sp->reqmax = get_fs_long ((int *) arg);
		    sp->state = SOCKM_LISTEN;

		    break;

		case 0x537b:    /*  SOACCEPT   */
		    err = verify_area (VERIFY_WRITE, (void *) arg,
							    sizeof (int));
		    if (err)  return err;

		    if (sp->state == SOCKM_CONN ||
			sp->state == SOCKM_DISCONN ||
			sp->state == SOCKM_REQUEST
		    )  return -EMLINK;      /*  svr3 behavior  */

		    save_flags (flags);
		    cli();

		    while (sp->connq == NULL) {

			if (filp->f_flags & O_NONBLOCK) {
			    restore_flags (flags);
			    return -EDEADLK;    /*  svr3 behavior (brrr...)  */
			}

			interruptible_sleep_on (&sp->wait);

			if (current->signal & ~current->blocked) {
			    restore_flags (flags);
			    return -ERESTARTSYS;
			}
		    }

		    req = sp->connq;
		    sp->connq = req->connq;

		    sp->reqcnt--;
#if 0
		    req->connq = sp;    /* ???  */
#endif
		    restore_flags (flags);

		    put_fs_long (req->num, (int *) arg);

		    break;

		case 0x537c:    /*  SOCONNECT   */
		    err = verify_area (VERIFY_READ, (void *) arg, 64);
		    if (err)  return err;

		    if (sp->state == SOCKM_CONN ||
			sp->state == SOCKM_DISCONN ||
			sp->state == SOCKM_REQUEST ||
			sp->state == SOCKM_LISTEN
		    )  return -EMLINK;      /*  svr3 behavior  */

		    memcpy_fromfs (name, (void *) arg, 64);
		    name[64] = '\0';

		    for (i = 0; i < 255; i++)
			if (sockm_base[i] &&
			    sockm_base[i]->name[0] &&
			    !strcmp (sockm_base[i]->name, name)
			)  break;
		    if (i >= 255)  return -ENOENT;  /*  svr3 behavior  */

		    serv = sockm_base[i];

		    if (serv->state != SOCKM_LISTEN)
			    return -EINVAL;     /*  svr3 behavior  */

		    save_flags (flags);
		    cli();

		    if (serv->reqcnt >= serv->reqmax) {
			restore_flags (flags);
			return -EINVAL;     /*  svr3 behavior  */
		    }

		    for (i = 0; i < 255 && sockm_base[i]; i++) ;
		    if (i >= 255) {
			restore_flags (flags);
			return -EBUSY;
		    }

		    sockm_base[i] = kmalloc (sizeof (struct sockm),
							    GFP_KERNEL);
		    if (sockm_base[i] == NULL) {
			restore_flags (flags);
			return -ENOMEM;
		    }

		    req = sockm_base[i];
		    memset (req, 0, sizeof (struct sockm));
		    req->num = i;
		    req->state = SOCKM_REQUEST;

		    serv->reqcnt++;

		    req->peer = sp->num;
		    sp->peer = req->num;
		    sp->state = SOCKM_CONN;

		    req->connq = serv->connq;
		    serv->connq = req;

		    wake_up_interruptible (&serv->wait);

		    restore_flags (flags);

		    break;

		case 0x537e:    /*  SVRSELECT   */
		    return -ENOSYS;     /*  no more kludge way under Linux  */
		    break;

		case 0x537f:    /*  SVWSELECT   */
		    return -ENOSYS;     /*  no more kludge way under Linux  */
		    break;

		case FIONBIO:   /*  renumber to linux stile by svr3_ioctl  */
		    return 0;   /*  should be catched by high level   */
		    break;

		case FIONREAD:  /*  renumber to linux stile by svr3_ioctl  */
		    err = verify_area (VERIFY_WRITE, (void *) arg,
							    sizeof (int));
		    if (err)  return err;

		    if (sp->state == SOCKM_CONN || sp->state == SOCKM_LISTEN) {
			int n = sp->tail - sp->head;

			if (n < 0)  n += PAGE_SIZE;
			put_fs_long (n, (int *) arg);
		    } else
			put_fs_long (0, (int *) arg);

		    break;

		default:
		    return -EINVAL;
		    break;
	    }

	return 0;
}

static int sockm_select (struct inode *inode, struct file *filp,
					int sel_type, select_table *wait) {
	int minor = MINOR (inode->i_rdev);
	struct sockm *sp = sockm_base[minor];

	if (sel_type == SEL_IN) {       /*  SVRSELECT   */

	    if (sp->cnt == 0 &&
		(sp->state == SOCKM_CONN ||
		 (sp->state == SOCKM_LISTEN && sp->connq == NULL))
	    ) {
		select_wait (&sp->wait, wait);
		return 0;
	    }

	    return 1;
	}
	else if (sel_type == SEL_OUT) {     /*  SVWSELECT   */

	    if (sp->state == SOCKM_LISTEN)  return 0;   /* ???  */

	    if (sp->state == SOCKM_CONN &&
		sockm_base[sp->peer]->cnt == PAGE_SIZE
	    ) {
		select_wait (&sp->wait, wait);
		return 0;
	    }

	    return 1;
	}

	/*  SEL_EX   */
	return 0;
}
