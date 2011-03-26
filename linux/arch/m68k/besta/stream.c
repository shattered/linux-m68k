/*
 * besta/stream.c -- Emulation of sysv`s high level stream interface.
 *		     This may be used to make sysv programs happy, but
 *		     an appropriate "stream module" realisation is needed too
 *		     (like a `besta/sockmod.c' etc.).
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

#include <asm/system.h>
#include <asm/page.h>

#include "stream.h"


static int stream_open (struct inode *inode, struct file *filp);
static void stream_release (struct inode *inode, struct file *filp);
static int stream_read (struct inode *inode, struct file *filp,
						char *buf, int count);
static int stream_write (struct inode *inode, struct file *filp,
					    const char * buf, int count);
static int stream_select (struct inode *inode, struct file *filp,
					int sel_type, select_table *wait);
static int stream_ioctl (struct inode *inode, struct file *filp,
					unsigned int cmd, unsigned long arg);

static struct file_operations stream_fops = {
	NULL,
	stream_read,
	stream_write,
	NULL,
	stream_select,
	stream_ioctl,
	NULL,
	stream_open,
	stream_release,
	NULL,
	NULL,
	NULL,
	NULL,
};


static int stream_getmsg (struct file *filp, struct strbuf *ctlptr,
					struct strbuf *dataptr, int *flags);
static int stream_putmsg (struct file *filp, struct strbuf *ctlptr,
					struct strbuf *dataptr, int flags);
static struct module_operations *find_module (char *name);

extern void sockmod_init (void);

void stream_init (const char *name, int major) {
	static int called = 0;

	if (register_chrdev (major, name, &stream_fops)) {
		printk ("Unable to get major %d\n", major);
		return;
	}

	if (!called) {      /*  may be called several times...  */
	    called = 1;

	    stream_getmsg_func = stream_getmsg;
	    stream_putmsg_func = stream_putmsg;

	    sockmod_init();
	}

	return;
}


static int stream_open (struct inode *inode, struct file *filp) {
	struct stream_info *stream_info;

	if (inode->i_count == 1) {  /*  first open for this inode   */
	    stream_info = (struct stream_info *)
				kmalloc (sizeof (*stream_info), GFP_KERNEL);
	    if (!stream_info)  return -ENOMEM;

	    memset (stream_info, 0, sizeof (*stream_info));
	    inode->u.generic_ip = stream_info;
	}

	if (!inode->u.generic_ip) {
	    printk ("stream (%d,%d): i_count=%d, stream_info = NULL\n",
		  MAJOR (inode->i_rdev), MINOR (inode->i_rdev), inode->i_count);
	    return -ENOSR;  /* ???  */
	}

	/*  Assume no "modules" in stream. Return success.   */

	return 0;
}

static void stream_release (struct inode *inode, struct file *filp) {
	struct stream_info *stream_info = inode->u.generic_ip;

	if (inode->i_count == 1) {  /*  last close on this inode   */

	    if (stream_info->m_ops && stream_info->m_ops->close)
		    stream_info->m_ops->close (stream_info, filp);

	    kfree (stream_info);
	    inode->u.generic_ip = NULL;
	}

	return;
}


/*  stream  ioctl  stuff   */

static int stream_peek (struct stream_info *stream_info, struct strpeek *arg) {
	int err;
	struct strpeek strpeek;

	if (!stream_info->m_ops ||
	    !stream_info->m_ops->peekmsg
	)  return -EINVAL;

	/*  read & write   */
	err = verify_area (VERIFY_WRITE, arg, sizeof (strpeek));
	if (err)  return err;
	memcpy_fromfs (&strpeek, arg, sizeof (strpeek));

	if (strpeek.flags & ~0x1)  return -EINVAL;

	if (strpeek.ctlbuf.maxlen > 0) {
	    err = verify_area (VERIFY_WRITE, strpeek.ctlbuf.buf,
						strpeek.ctlbuf.maxlen);
	    if (err)  return err;
	}

	if (strpeek.databuf.maxlen > 0) {
	    err = verify_area (VERIFY_WRITE, strpeek.databuf.buf,
						strpeek.databuf.maxlen);
	    if (err)  return err;
	}

	strpeek.ctlbuf.len = strpeek.ctlbuf.maxlen;
	strpeek.databuf.len = strpeek.databuf.maxlen;

	err = stream_info->m_ops->peekmsg (stream_info,
				strpeek.ctlbuf.buf, &strpeek.ctlbuf.len,
				strpeek.databuf.buf, &strpeek.databuf.len,
					&strpeek.flags);
	if (err < 0)  return err;

	memcpy_tofs (arg, &strpeek, sizeof (strpeek));

	return err;
}

static int stream_fdinsert (struct stream_info *stream_info,
				struct file *filp, struct strfdinsert *arg) {
	int err;
	struct strfdinsert strfdinsert;
	struct file *ins_file;
	int nonblocks = !!(filp->f_flags & O_NONBLOCK);

	if (!stream_info->m_ops ||
	    !stream_info->m_ops->putmsg
	)  return -EINVAL;

	err = verify_area (VERIFY_READ, arg, sizeof (strfdinsert));
	if (err)  return err;

	memcpy_fromfs (&strfdinsert, arg, sizeof (strfdinsert));

	/*  checking...  */
	if (strfdinsert.ctlbuf.len < sizeof (void *) ||
	    strfdinsert.ctlbuf.len > CTL_MAX ||
	    (strfdinsert.offset & 0x1) ||   /*  word aligned...  */
	    strfdinsert.offset > strfdinsert.ctlbuf.len - sizeof (void *) ||
	    (strfdinsert.flags & ~0x1)
	)  return -EINVAL;

	if (strfdinsert.fildes >= NR_OPEN ||
	    !(ins_file = current->files->fd[strfdinsert.fildes]) ||
	    ins_file->f_op != &stream_fops
	)  return -EINVAL;

	/*  read & write   */
	err = verify_area (VERIFY_WRITE, strfdinsert.ctlbuf.buf,
						strfdinsert.ctlbuf.len);
	if (err)  return err;

	if (strfdinsert.databuf.len > 0) {
	    if (strfdinsert.databuf.len < stream_info->min ||
		strfdinsert.databuf.len > stream_info->max
	    )  return -ERANGE;

	    err = verify_area (VERIFY_READ, strfdinsert.databuf.buf,
						strfdinsert.databuf.len);
	    if (err)  return err;
	}

	/*  it is not very good to insert file struct ptr into user space,
	   but it is so easy...
	*/
	put_user ((void *) ins_file,
		   (void **) (strfdinsert.ctlbuf.buf + strfdinsert.offset));


	/*  We are sending now -- the lo-o-ove message to the Wo-orld,
	    the lo-o-ove message to the Wo-orld,
	    the lo-o-ove message to the Wo-orld!  (LOVE  MESSAGE !)
	  Lo-ove message, to-o feeling,
	  Lo-ove message, to-o feeling,
	  Lo-ove message, lo-o-ove message to the Wo-orld !
	    LOVE  MESSAGE  !!!
	    SAVE !
	    YOUR !
	    LIFE !!!

	    YE-E-A !!!!!!!!!
	*/

	err = stream_info->m_ops->putmsg (stream_info,
			    strfdinsert.ctlbuf.buf, strfdinsert.ctlbuf.len,
			    strfdinsert.databuf.buf, strfdinsert.databuf.len,
				nonblocks, strfdinsert.flags);
	return err;
}


static int stream_ioctl (struct inode *inode, struct file *filp,
					unsigned int cmd, unsigned long arg) {
	int err, len, value;
	char *name;
	struct module_operations *mod_ops, *old_mod_ops;
	struct stream_info *stream_info = inode->u.generic_ip;
	struct strioctl strioctl;
	struct strrecvfd strrecvfd;
	char fill[8];
	int uid, gid, timeout;

	switch (cmd) {

	    case I_PUSH:
		err = getname ((char *) arg, &name);
		if (err)  return err;

		/*  find a stream module for this name   */
		mod_ops = find_module (name);
		putname (name);
		if (!mod_ops)  return -EINVAL;

		/*  insert new stream module   */
		old_mod_ops = stream_info->m_ops;
		stream_info->m_ops = mod_ops;

		stream_info->max = 0;
		stream_info->min = 0;

		/*  call a stream module private open routine   */
		err = stream_info->m_ops->open (stream_info, filp, old_mod_ops);
		if (err) {
			stream_info->m_ops = NULL;
			return err;
		}

		if (stream_info->max == 0)
			/*  stream module do`nt want to use this feature,
			   so, assume it should be non-limit...  */
			stream_info->max = 0xffffffff;

		return err;
		break;

	    case I_FIND:
		if (!stream_info->m_ops)  return 0;

		err = getname ((char *) arg, &name);
		if (err)  return err;

		/*  check, is it a current module   */
		if (!strcmp (name, stream_info->m_ops->name)) {
			putname (name);
			return 1;
		}

		/*  current module may emulate several "sysv" modules...  */
		if (stream_info->m_ops->find)
		    err = stream_info->m_ops->find (stream_info, name);
		else
		    err = 0;

		putname (name);

		return err;
		break;

	    case I_LOOK:
		if (!stream_info->m_ops)  return -EINVAL;

		len = strlen (stream_info->m_ops->name) + 1;

		err = verify_area (VERIFY_WRITE, (void *) arg, len);
		if (err)  return err;

		memcpy_tofs ((void *) arg, stream_info->m_ops->name, len);

		return 0;
		break;

	    case I_POP:
		if (!stream_info->m_ops)  return -EINVAL;

		if (stream_info->m_ops->close)
		    stream_info->m_ops->close (stream_info, filp);

		else
		    /*  module clears this field to determine `that`s all'  */
		    stream_info->m_ops = NULL;

		return 0;
		break;

	    case I_SRDOPT:
		switch (arg) {
		    case RNORM:
		    case RMSGD:
		    case RMSGN:
			stream_info->read_opts = arg;
			break;
		    default:
			return -EINVAL;
			break;
		}

		return 0;
		break;

	    case I_GRDOPT:
		err = verify_area (VERIFY_WRITE, (void *) arg, sizeof (int));
		if (err)  return err;

		put_user (stream_info->read_opts, (int *) arg);

		return 0;
		break;

	    case I_SETSIG:
		if (arg & ~(S_INPUT | S_HIPRI | S_OUTPUT | S_MSG))
			return -EINVAL;

		if (!arg)
		    stream_info->sig_mask |= arg;
		else if (stream_info->sig_mask)
			stream_info->sig_mask = 0;
		else
		    return -EINVAL;

		return 0;
		break;

	    case I_GETSIG:
		if (!stream_info->sig_mask)  return -EINVAL;

		err = verify_area (VERIFY_WRITE, (void *) arg, sizeof (int));
		if (err)  return err;

		put_user (stream_info->poll_mask, (int *) arg);

		return 0;
		break;

	    case I_STR:
		if (!stream_info->m_ops ||
		    !stream_info->m_ops->ioctl
		)  return -EINVAL;

		/*  read & write   */
		err = verify_area (VERIFY_WRITE, (void *) arg,
						    sizeof (strioctl));
		if (err)  return err;

		memcpy_fromfs (&strioctl, (void *) arg, sizeof (strioctl));

		if (strioctl.ic_len < 0)  return -EINVAL;

		if (strioctl.ic_timeout > 0)
			timeout = strioctl.ic_timeout * HZ;
		else if (strioctl.ic_timeout == 0)
			timeout = 15 * HZ;
		else if (strioctl.ic_timeout == -1)
			timeout = 0;
		else
		    return -EINVAL;

		if (strioctl.ic_len) {
		    /*  read & write  (but sometimes may be only read ?)  */
		    err = verify_area (VERIFY_WRITE, strioctl.ic_dp,
							    strioctl.ic_len);
		    if (err)  return err;
		}

		err = stream_info->m_ops->ioctl (stream_info,
						   strioctl.ic_cmd,
						     strioctl.ic_dp,
						       &strioctl.ic_len,
							 timeout);
		if (err >= 0)
		    memcpy_tofs ((void *) arg, &strioctl, sizeof (strioctl));

		return err;
		break;

	    case 0x541b:    /*  FIONREAD, svr3`s own kludge behavior...  */
	    case I_NREAD:
		if (!stream_info->m_ops ||
		    !stream_info->m_ops->nread
		)  return -EINVAL;

		err = verify_area (VERIFY_WRITE, (int *) arg, sizeof (int));
		if (err)  return err;

		err = stream_info->m_ops->nread (stream_info, &value);
		if (err < 0)  return err;

		put_user (value, (int *) arg);

		return err;
		break;

	    case I_FLUSH:
		if (!stream_info->m_ops ||
		    !stream_info->m_ops->flush
		)  return 0;

		switch (arg) {
		    case FLUSHR:
			return  stream_info->m_ops->flush (stream_info, 0);
			break;
		    case FLUSHW:
			return  stream_info->m_ops->flush (stream_info, 1);
			break;
		    case FLUSHRW:
			err = stream_info->m_ops->flush (stream_info, 0);
			if (err)  return err;
			return  stream_info->m_ops->flush (stream_info, 1);
			break;
		    default:
			return -EINVAL;
			break;
		}
		break;

	    case I_LINK:
		if (!stream_info->m_ops ||
		    !stream_info->m_ops->link
		)  return -EINVAL;

		return  stream_info->m_ops->link (stream_info, filp, arg);
		break;

	    case I_UNLINK:
		if (!stream_info->m_ops ||
		    !stream_info->m_ops->unlink
		)  return -EINVAL;

		return  stream_info->m_ops->unlink (stream_info, filp, arg);
		break;

	    case I_PEEK:
		return  stream_peek (stream_info, (struct strpeek *) arg);
		break;

	    case I_SENDFD:
		if (!stream_info->m_ops ||
		    !stream_info->m_ops->sendfd
		)  return -EINVAL;

		if (arg >= NR_OPEN ||
		    !(current->files->fd[arg])
		)  return -EBADF;

		return  stream_info->m_ops->sendfd (stream_info, filp, arg);
		break;

	    case I_RECVFD:
		if (!stream_info->m_ops ||
		    !stream_info->m_ops->recvfd
		)  return -EINVAL;

		err = verify_area (VERIFY_WRITE, (void *) arg,
						    sizeof (strrecvfd));
		if (err)  return err;

		err = stream_info->m_ops->recvfd (stream_info, filp,
							&uid, &gid, fill);
		if (err < 0)  return err;

		strrecvfd.fd = err;
		strrecvfd.uid = uid;
		strrecvfd.gid = gid;
		memcpy (strrecvfd.fill, fill, sizeof (strrecvfd.fill));

		memcpy_tofs ((void *) arg, &strrecvfd, sizeof (strrecvfd));

		return 0;
		break;

	    case I_FDINSERT:
		return  stream_fdinsert (stream_info, filp,
					    (struct strfdinsert *) arg);
		break;

	    default:
		if (((cmd >> 8) & 0xff) == 'T' &&
		    stream_info->m_ops &&
		    stream_info->m_ops->ioctl
		) {
		    value = 0;      /*  know nothing about size...   */

		    return  stream_info->m_ops->ioctl (stream_info,
						cmd, (void *) arg, &value, 0);
		}

#if 0
		printk ("(%s): bad stream ioctl %08x, %08x\n",
					current->comm, cmd, arg);
#endif

		return -EINVAL;
		break;
	}

	return 0;   /*  not reached   */
}


static int stream_read (struct inode *inode, struct file *filp,
						char *buf, int count) {
	int total, err;
	struct stream_info *stream_info = inode->u.generic_ip;
	int nonblocks = !!(filp->f_flags & O_NONBLOCK);
	int flags, ctllen;

	/*  don`t know why, but intuition...  */
	if (!stream_info->m_ops)  return 0;

	switch (stream_info->read_opts) {

	    case RNORM:
		if (!stream_info->m_ops->read)  return -EINVAL;

		return  stream_info->m_ops->read (stream_info, buf, count,
								nonblocks);
		break;

	    case RMSGD:
		if (!stream_info->m_ops->getmsg)  return -EINVAL;

		total = count;
		flags = 0;
		ctllen = -1;

		err = stream_info->m_ops->getmsg (stream_info, NULL, &ctllen,
						buf, &total, nonblocks, &flags);
		if (err < 0)  return err;

		if (total < 0)  total = 0;

		if (err & MOREDATA) {   /*  should disgard this...   */
		    void *page = (void *) get_free_page (GFP_KERNEL);
		    int len;

		    if (!page)  return total;

		    do {
			len = PAGE_SIZE;

			set_fs (KERNEL_DS);
			err = stream_info->m_ops->getmsg (stream_info,
					NULL, &ctllen, page, &len, 1, &flags);
			set_fs (USER_DS);
			if (err < 0)  break;

		    } while (err & MOREDATA);

		    free_page ((int) page);
		}

		return total;
		break;

	    case RMSGN:
		if (!stream_info->m_ops->getmsg)  return -EINVAL;

		total = count;
		flags = 0;
		ctllen = -1;

		err = stream_info->m_ops->getmsg (stream_info, NULL, &ctllen,
						buf, &total, nonblocks, &flags);
		if (err < 0)  return err;

		return  total > 0 ? total : 0;
		break;
	}

	return 0;       /*  not  reached  */
}


static int stream_write (struct inode *inode, struct file *filp,
					    const char * buf, int count) {
	struct stream_info *stream_info = inode->u.generic_ip;
	int nonblocks = !!(filp->f_flags & O_NONBLOCK);
	int err, len, total = 0;

	/*  don`t know why, but intuition...  */
	if (!stream_info->m_ops)  return 0;
	if (!stream_info->m_ops->putmsg)  return -EINVAL;

	if (count < stream_info->min ||
	    (stream_info->min > 0 && count > stream_info->max)
	)  return -ERANGE;

	do {
	    len = count < stream_info->max ? count : stream_info->max;

	    err = stream_info->m_ops->putmsg (stream_info, NULL, -1,
					(char *) buf, len, nonblocks, 0);
	    if (err < 0)  return total ? total : err;

	    buf += len;
	    total += len;
	    count -= len;

	} while (count > 0);

	return total;
}


/*  getmsg/putmsg  stuff   */

static int stream_getmsg (struct file *filp, struct strbuf *ctlptr,
					struct strbuf *dataptr, int *flags) {
	int err;
	struct stream_info *stream_info;
	struct strbuf ctlbuf, databuf;
	int prio_flags;
	int nonblocks = !!(filp->f_flags & O_NONBLOCK);

	/*  first checking, if it is really a `stream' device...  */
	if (filp->f_op != &stream_fops)  return -ENOSTR;

	/*  what should we do in this case ?  */
	if (!filp->f_inode ||
	    !(stream_info = filp->f_inode->u.generic_ip) ||
	    !stream_info->m_ops ||
	    !stream_info->m_ops->getmsg
	)  return -EINVAL;

	if (ctlptr) {
	    /*  read & write   */
	    err = verify_area (VERIFY_WRITE, ctlptr, sizeof (*ctlptr));
	    if (err)  return err;

	    memcpy_fromfs (&ctlbuf, ctlptr, sizeof (*ctlptr));
	} else
	    ctlbuf.maxlen = -1;

	if (dataptr) {
	    /*  read & write   */
	    err = verify_area (VERIFY_WRITE, dataptr, sizeof (*dataptr));
	    if (err)  return err;

	    memcpy_fromfs (&databuf, dataptr, sizeof (*dataptr));
	} else
	    databuf.maxlen = -1;

	/*  read & write   */
	err = verify_area (VERIFY_WRITE, flags, sizeof (*flags));
	if (err)  return err;

	prio_flags = get_user (flags);
	if (prio_flags & ~0x1)  return -EINVAL;

	if (ctlbuf.maxlen >= 0) {
	    err = verify_area (VERIFY_WRITE, ctlbuf.buf, ctlbuf.maxlen);
	    if (err)  return err;
	}
	if (databuf.maxlen >= 0) {
	    err = verify_area (VERIFY_WRITE, databuf.buf, databuf.maxlen);
	    if (err)  return err;
	}

	ctlbuf.len = ctlbuf.maxlen;
	databuf.len = databuf.maxlen;

	err = stream_info->m_ops->getmsg (stream_info, ctlbuf.buf, &ctlbuf.len,
			    databuf.buf, &databuf.len, nonblocks, &prio_flags);
	if (err < 0)  return err;

	if (prio_flags)  prio_flags = 0x1;
	else  prio_flags = 0;
	put_user (prio_flags, flags);

	if (ctlptr)  memcpy_tofs (ctlptr, &ctlbuf, sizeof (*ctlptr));
	if (dataptr)  memcpy_tofs (dataptr, &databuf, sizeof (*dataptr));

	return err;
}


static int stream_putmsg (struct file *filp, struct strbuf *ctlptr,
					struct strbuf *dataptr, int flags) {
	int err;
	struct stream_info *stream_info;
	struct strbuf ctlbuf, databuf;
	int nonblocks = !!(filp->f_flags & O_NONBLOCK);

	/*  first checking, if it is really a `stream' device...  */
	if (filp->f_op != &stream_fops)  return -ENOSTR;

	/*  what should we do in this case ?  */
	if (!filp->f_inode ||
	    !(stream_info = filp->f_inode->u.generic_ip) ||
	    !stream_info->m_ops ||
	    !stream_info->m_ops->putmsg
	)  return -EINVAL;

	if (flags & ~0x1)  return -EINVAL;

	if (ctlptr) {
	    err = verify_area (VERIFY_READ, ctlptr, sizeof (*ctlptr));
	    if (err)  return err;

	    memcpy_fromfs (&ctlbuf, ctlptr, sizeof (*ctlptr));
	} else
	    ctlbuf.len = -1;

	if (dataptr) {
	    err = verify_area (VERIFY_READ, dataptr, sizeof (*dataptr));
	    if (err)  return err;

	    memcpy_fromfs (&databuf, dataptr, sizeof (*dataptr));
	} else
	    databuf.len = -1;

	if (ctlbuf.len < 0 && (flags & 0x1))  return -EINVAL;
	if (ctlbuf.len < 0 && databuf.len < 0)  return 0;

	if (ctlbuf.len >= 0) {
	    if (ctlbuf.len > CTL_MAX)  return -ERANGE;

	    err = verify_area (VERIFY_READ, ctlbuf.buf, ctlbuf.len);
	    if (err)  return err;
	}
	if (databuf.len >= 0) {
	    if (databuf.len < stream_info->min ||
		databuf.len > stream_info->max
	    )  return -ERANGE;

	    err = verify_area (VERIFY_READ, databuf.buf, databuf.len);
	    if (err)  return err;
	}

	err = stream_info->m_ops->putmsg (stream_info, ctlbuf.buf, ctlbuf.len,
						    databuf.buf, databuf.len,
							nonblocks, flags);
	return err;
}


static int stream_select (struct inode *inode, struct file *filp,
					int sel_type, select_table *wait) {
	struct stream_info *stream_info = inode->u.generic_ip;

	if (!stream_info->m_ops->select)  return 1;

	return  stream_info->m_ops->select (stream_info, filp, sel_type, wait);
}


/*   stream  modules  stuff   */

struct module_operations *mod_op_base = NULL;

static struct module_operations *find_module (char *name) {
	struct module_operations *m_op;

	for (m_op = mod_op_base; m_op; m_op = m_op->next)
		if (!strcmp (m_op->name, name))  break;

	return m_op;
}

int reguster_stream_module (struct module_operations *new) {
	struct module_operations *m_op;

	if (!mod_op_base)  mod_op_base = new;
	else {
	    for (m_op = mod_op_base; m_op->next; m_op = m_op->next) ;

	    m_op->next = new;
	}
	new->next = NULL;

	return 0;
}

int unregister_stream_module (struct module_operations *old) {
	struct module_operations *m_op;

	for (m_op = mod_op_base; m_op && m_op != old; m_op = m_op->next)

	if (!m_op)  return -ENOENT;

	if (old == mod_op_base) {
		mod_op_base = old->next;
		return 0;
	}

	for (m_op = mod_op_base; m_op->next != old; m_op = m_op->next) ;
	m_op->next = old->next;

	return 0;
}


