/*
 * besta/xfd.c -- Floppy driver for HCPU30 board.
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

#include <linux/blkdev.h>
#include <linux/locks.h>
#include <linux/config.h>
#include <linux/errno.h>
#include <linux/signal.h>
#include <linux/sched.h>
#include <linux/timer.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/genhd.h>
#include <linux/delay.h>
#include <linux/config.h>
#include <linux/mm.h>

#include <linux/interrupt.h>

#include <asm/pgtable.h>
#include <asm/system.h>
#include <asm/segment.h>

#include "besta.h"
#include "hcpu30.h"

#define XFD_MAJOR       48

#define MAX_ERRORS      8       /* Max read/write errors/sector */
#define MAX_LUN         8       /* Max LUNs per target */
#define MAX_DEV         5       /* Max number of real  */
#define MAX_RETRY       3

#define STATE_FREE      0
#define STATE_IO        1
#define STATE_CMD       2
#define STATE_FORMAT    3
#define STATE_RETRY     4
#define STATE_RETRY_FORMAT      5
#define STATE_BREAK     6

#define SECTOR_MASK (blksize_size[XFD_MAJOR] &&     \
	blksize_size[XFD_MAJOR][MINOR(curr->rq_dev)] ? \
	((blksize_size[XFD_MAJOR][MINOR(curr->rq_dev)] >> 9) - 1) :  \
	((BLOCK_SIZE >> 9)  -  1))

static int xfd_open (struct inode *inode, struct file *filp);
static void xfd_release (struct inode *inode, struct file *file);
static void do_xfd_request (void);
static void xfd_intr (int vec, void *data, struct pt_regs *fp);
static void end_request (int uptodate, int major);
static int xfd_ioctl( struct inode *inode, struct file *file, unsigned int
		       cmd, unsigned long arg );
static int do_xfd_cmd (int dev, int cmd);
static int do_xfd_format (int dev);


static struct file_operations xfd_fops = {
	NULL,                   /* lseek - default */
	block_read,             /* read - general block-dev read */
	block_write,    /* write - general block-dev write */
	NULL,                   /* readdir - bad */
	NULL,                   /* select */
	xfd_ioctl,             /* ioctl */
	NULL,                   /* mmap */
	xfd_open,              /* open */
	xfd_release,   /* release */
	block_fsync,            /* fsync */
	NULL,                   /* fasync */
	NULL,                   /* media_change */
	NULL,                   /* revalidate */
};

struct xfd {
	unsigned char   x0;
	unsigned char   x1;
	unsigned short  x2;
	void           *x4;
	unsigned char   x8;
	unsigned char   x9;
	unsigned char   xa;
	unsigned char   xb;
	unsigned char   xc;
	unsigned char   xd;
	unsigned char   xe;
	char            rf;
	char            filler[16];
};

struct xfdpar {
	unsigned char  f_heads;
	unsigned char  f_cyl;
	unsigned char  f_sptrack;
	unsigned char  f_firstsec;
	unsigned char  f_bps;
	unsigned char  f_dens;
	unsigned char  f_x6;
	unsigned char  f_formpattern;
	unsigned char  f_formpattern_h;
	unsigned char  f_interleave;
	unsigned char  f_steprate;     /* 0=6ms, 1=12ms, 2=2ms, 3=3ms  */
	unsigned char  f_xb;

} xfdpar[] = {

/*auto*/{ 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 },
/*800*/ { 0x2,0x50, 0x5, 0x0, 0x0, 0x0, 0x5,0xe5,0x30, 0x0, 0x3, 0x0 },
/*1200*/{ 0x2,0x50, 0xf, 0x1, 0x1, 0x1, 0x5,0xe5,0x30, 0x0, 0x3, 0x0 },
/*720*/ { 0x2,0x50, 0x9, 0x0, 0x1, 0x0, 0x5,0xe5,0x30, 0x0, 0x3, 0x0 },
/*360*/ { 0x2,0x28, 0x9, 0x1, 0x1, 0x0, 0x5,0xe5,0x30, 0x0, 0x3, 0x0 },
/*1440*/{ 0x2,0x50, 0x9, 0x0, 0x0, 0x1, 0x5,0xe5,0x30, 0x0, 0x3, 0x0 },
/*640*/ { 0x2,0x50,0x10, 0x1, 0x2, 0x0, 0x5,0xe5,0x30, 0x0, 0x3, 0x0 }
};

#define FD_MINORS (sizeof (xfdpar) / sizeof (struct xfdpar))

static struct hd_struct xfd_part[FD_MINORS] = {{0,0}, };
static int      xfd_blocksizes[FD_MINORS] = { 0, };

/*  Must be first not autoprobe minor.  */
#define FIRST_REAL_DEV  1
static int xfd_real_dev = FIRST_REAL_DEV;

static int xfd_access_count = 0;
static int xfd_busy = 0;
static int xfd_cmd_ret = 0;
static int xfd_retry = 0;

static struct wait_queue *cmd_wait = NULL;

static char xfdinterl[32];


void xfd_init (void) {
	volatile struct xfd *v = (struct xfd *) XFD_ADDR;
	int i;
	int vector, level;

	if (!besta_get_vect_lev ("xfd", &vector, &level)) {
		vector = get_unused_vector();
		level = XFD_LEV;
	}

	v->xe = vector;
	v->xd = level;

	for (i=0;i < FD_MINORS;i++) {
	    xfd_part[i].start_sect = 0;
	    xfd_part[i].nr_sects = (xfdpar[i].f_heads *
				    xfdpar[i].f_cyl *
				    xfdpar[i].f_sptrack) >> xfdpar[i].f_bps;
	    xfd_blocksizes[i] = 1024 >> xfdpar[i].f_bps;

	    /*  buffer.c don`t want secsize less than 512 bytes...  */
	    if (xfd_blocksizes[i] < 512)
		    xfd_blocksizes[i] = 512;
	}

	if (register_blkdev (XFD_MAJOR, "xfd", &xfd_fops)) {
	    printk ("Unable to get major %d \n",XFD_MAJOR);
	    return;
	}

	blk_dev[XFD_MAJOR].request_fn = do_xfd_request;
	read_ahead[XFD_MAJOR] = 0;
	blksize_size[XFD_MAJOR] = xfd_blocksizes;

	besta_handlers[vector] = xfd_intr;
	besta_intr_data[vector] = NULL;

	return;
}


static int xfd_open (struct inode *inode, struct file *filp) {
	int dev = MINOR (inode->i_rdev);
	int err, i;

	if (dev >= FD_MINORS)  return -ENXIO;

/*      if (filp && filp->f_mode)
		check_disk_change (inode->i_rdev);      */


	if (xfd_access_count > 0) {
		xfd_access_count++;
		return 0;
	}

	if (dev == 0) {  /*  autoprobing is needable   */
	    static int last_probing = 0;

	    /*  There are some reasons to probe all list each time,
		because formats PC720 & PC360 are very similar, etc.
		But there may be very intensive  `open/close'ing
		at the same small time period (i.e. -- `mount -t auto')
		For such cases we try to use last found format the first,
		while 4 seconds` time interval is not expired.
	    */

	    if (jiffies > last_probing + 4*HZ  ||
		(xfd_real_dev <= 0 && xfd_real_dev >= FD_MINORS)
	    )  xfd_real_dev = FIRST_REAL_DEV;

	    if (do_xfd_cmd (xfd_real_dev, 67) < 0 ||
		do_xfd_cmd (xfd_real_dev, 321) < 0
	    ) {   /*  Not the same as previous, try again all the list   */

		for (i = FIRST_REAL_DEV; i < FD_MINORS; i++) {
		    if (i == xfd_real_dev)  continue;   /*  has been probed */
		    if (do_xfd_cmd (i, 67) >= 0 &&
			do_xfd_cmd (i, 321) >= 0
		    )  break;
		}
		if (i == FD_MINORS)  return -ENXIO;

		xfd_real_dev = i;
		last_probing = jiffies;
	    }

	    /*  set true sectsize for autoprobed dev...  */
	    xfd_blocksizes[0] = xfd_blocksizes[xfd_real_dev];

	    xfd_access_count++;
	    return 0;
	}

	/* usual case  */
	err = do_xfd_cmd (dev, 67);
	if (err < 0)  return err;

	xfd_real_dev = dev;

	xfd_access_count++;
	return 0;
}

static void xfd_release (struct inode *inode, struct file *file) {

	fsync_dev(inode->i_rdev);

	xfd_access_count--;
	if (xfd_access_count == 0) {
	    invalidate_inodes(inode->i_rdev);
	    invalidate_buffers(inode->i_rdev);
	}

	return;
}

static void do_xfd_request (void) {
	volatile struct xfd *v = (struct xfd *) XFD_ADDR;
	int dev, sector, sec_on_track, nr_track, curr_nr_sects;
	struct buffer_head *bh;
	struct request *curr;

repeat:
	curr = blk_dev[XFD_MAJOR].current_request;

	if (!curr || curr->rq_status == RQ_INACTIVE)  return;

	if (MAJOR(curr->rq_dev) != XFD_MAJOR)
		panic ("xfd: request list destroyed !  ");

	bh = curr->bh;
	if (!bh || !buffer_locked (bh))
		panic ("xfd: block not locked !  ");

	dev = MINOR (curr->rq_dev);
	if (dev == 0)  dev = xfd_real_dev;      /*  autoprobed dev   */

	if (dev >= FD_MINORS ||
	    dev != xfd_real_dev ||
	    curr->sector + curr->nr_sectors > 2*xfd_part[dev].nr_sects
	) {
		end_request (0, XFD_MAJOR);
		goto repeat;
	}

	if (xfd_busy) return;

	sector = ((curr->sector +
		   2*xfd_part[dev].start_sect) << xfdpar[dev].f_bps) >> 1;
	sec_on_track = sector % xfdpar[dev].f_sptrack;
	nr_track = sector / xfdpar[dev].f_sptrack;

	curr_nr_sects = (curr->current_nr_sectors << xfdpar[dev].f_bps) >> 1;
	if (sec_on_track + curr_nr_sects > xfdpar[dev].f_sptrack)
		curr_nr_sects = xfdpar[dev].f_sptrack - sec_on_track;

	v->x1 = (xfdpar[dev].f_cyl == 80 ? 0 : 128) | xfdpar[dev].f_bps;
	v->x8 = xfdpar[dev].f_dens;
	v->x9 = 1;   /*  but may be 0 in some odd cases ???   */

	v->xc = sec_on_track + xfdpar[dev].f_firstsec;
	v->xb = nr_track % xfdpar[dev].f_heads;
	v->xa = nr_track / xfdpar[dev].f_heads;

	v->x2 = curr_nr_sects << (10 - xfdpar[dev].f_bps);

	xfd_retry = 0;
	xfd_busy = STATE_IO;

	v->x4 = curr->buffer;
	if (curr->cmd == WRITE)  v->x0 = 66;
	else if (curr->cmd == READ)  v->x0 = 65;
	else  panic ("xfd: unknown command");

	return;
}

static void xfd_intr (int vec, void *data, struct pt_regs *fp) {
	volatile struct xfd *v = (struct xfd *) XFD_ADDR;
	struct request *curr;

	curr = blk_dev[XFD_MAJOR].current_request;

	if (xfd_busy == STATE_FREE) {
		printk ("xfd: interrupt while inactive\n");
		return;
	}
	else if (xfd_busy == STATE_IO) {

	    if (v->x0 & 0x40) {  /*  byaka   */

		if (xfd_retry++ < MAX_RETRY) {
		    v->x0 = 68;
		    xfd_busy = STATE_RETRY;
		    return;
		} else {
		    printk ("xfd: error sta=%x FDC error (STATE_IO)"
			    "cyl=%d head=%d sector=%d\n",
			    v->x0, v->xa, v->xb, v->xc);
		    end_request (0, XFD_MAJOR);
		}

	    } else {
		int n = v->x2 >> 9;     /* 512-byte blocks  */

		curr->nr_sectors -= n;
		curr->current_nr_sectors -= n;
		curr->sector += n;
		curr->buffer += v->x2;  /*  be careful: clear_data_cache()
					   in the end_request() use this... */

		if (curr->current_nr_sectors > 0) {
		    xfd_busy = STATE_FREE;
		    do_xfd_request ();
		    return;
		}

		if (curr->bh || curr->nr_sectors <= 0)
			end_request (1, XFD_MAJOR);
	    }

	    xfd_busy = STATE_FREE;
	}
	else if (xfd_busy == STATE_RETRY) {

	    if (v->x0 & 0x40) {  /*  byaka in come back moving  */
		printk ("xfd: error sta=%x FDC error (STATE_RETRY)"
			"cyl=%d head=%d sector=%d\n",
			v->x0, v->xa, v->xb, v->xc);
		end_request (0, XFD_MAJOR);

		xfd_busy = STATE_FREE;
	    } else {

		if (curr->cmd == WRITE)  v->x0 = 66;
		else if (curr->cmd == READ)  v->x0 = 65;

		xfd_busy = STATE_IO;
		return;
	    }
	}
	else if (xfd_busy == STATE_CMD) {

	    if (v->x0 & 0x40)  xfd_cmd_ret = -EIO;
	    else  xfd_cmd_ret = 0;

	    xfd_busy = STATE_FREE;
	}
	else if (xfd_busy == STATE_FORMAT) {

	    if (v->x0 & 0x40) {  /*  byaka   */

		if (xfd_retry++ < MAX_RETRY) {
		    v->x0 = 68;
		    xfd_busy = STATE_RETRY_FORMAT;
		    return;
		} else {
		    printk ("xfd: error sta=%x FDC error (STATE_FORMAT)"
			    "cyl=%d head=%d sector=%d\n",
			    v->x0, v->xa, v->xb, v->xc);
		    xfd_cmd_ret = -EIO;
		}

	    } else {

		v->xb += 1;
		if (v->xb == xfdpar[xfd_real_dev].f_heads) {
		    v->xb = 0;
		    v->xa += 1;
		}
		if (v->xa != xfdpar[xfd_real_dev].f_cyl) {
		    v->x2 = xfdpar[xfd_real_dev].f_formpattern_h << 8 |
				    xfdpar[xfd_real_dev].f_formpattern;
		    xfd_retry = 0;      /* restart retry for new track  */

		    v->x0 = 69;
		    return;
		}
		xfd_cmd_ret = 0;
	    }

	    xfd_busy = STATE_FREE;
	}
	else if (xfd_busy == STATE_RETRY_FORMAT) {

	    if (v->x0 & 0x40) {  /*  byaka   */

		printk ("xfd: error sta=%x FDC error (STATE_RETRY_FORMAT)"
			"cyl=%d head=%d sector=%d\n",
			v->x0, v->xa, v->xb, v->xc);
		xfd_cmd_ret = -EIO;

	    } else {
		/*  Is it really needable hear ???   */
		v->x2 = xfdpar[xfd_real_dev].f_formpattern_h << 8 |
				xfdpar[xfd_real_dev].f_formpattern;
		v->x0 = 69;
		xfd_busy = STATE_FORMAT;
		return;
	    }

	    xfd_busy = STATE_FREE;
	}
	else if (xfd_busy == STATE_BREAK) {
	    xfd_busy = STATE_FREE;
	}
	else panic ("Bad xfd_busy type ");

	if (waitqueue_active (&cmd_wait))
		wake_up_interruptible (&cmd_wait);

	/*  always run request queue   */
	do_xfd_request ();

	return;
}

static int do_xfd_cmd (int dev, int cmd) {
	volatile struct xfd *v = (struct xfd *) XFD_ADDR;
	unsigned short flags;
	char xfd_buffer[1024];

	save_flags (flags);
	cli();

	while (xfd_busy != STATE_FREE) {
	    interruptible_sleep_on (&cmd_wait);

	    if (current->signal & ~current->blocked) {
		    restore_flags (flags);
		    return -ERESTARTSYS;
	    }
	}
	xfd_busy = STATE_CMD;

	v->x1 = (xfdpar[dev].f_cyl == 80 ? 0 : 128) | xfdpar[dev].f_bps;
	v->x8 = xfdpar[dev].f_dens;
	v->x9 = 1;  /*  but may be 0 in some odd cases ???   */

	if (cmd == 67) {

	    /*  especial for cmd=67   */
	    v->x2 = xfdpar[dev].f_steprate << 4;
	}
	else if (cmd == 321) {   /*  read probe   */

	    v->xc = 0 + xfdpar[dev].f_firstsec;
	    v->xb = 0;
	    v->xa = 0;

	    v->x2 = 1 << (10 - xfdpar[dev].f_bps);
	    v->x4 = xfd_buffer;

	    cmd = 65;
	}
	else {
		xfd_busy = STATE_FREE;
		restore_flags (flags);
		printk ("xfd: unknown special cmd %d\n",cmd);
		return  -ENOSYS;
	}

	xfd_retry = 0;
retry:
	xfd_busy = STATE_CMD;   /* needable hear because retry label  */
	v->x0 = cmd;

	interruptible_sleep_on (&cmd_wait);
	if (xfd_busy != STATE_FREE) {
		xfd_busy = STATE_BREAK;
		restore_flags (flags);
		return -EINTR;
	}

	if (xfd_cmd_ret != 0) {

	    if (xfd_retry++ < MAX_RETRY) {
		xfd_busy = STATE_CMD;
		v->x0 = 68;

		interruptible_sleep_on (&cmd_wait);
		if (xfd_busy != STATE_FREE) {
			xfd_busy = STATE_BREAK;
			restore_flags (flags);
			return -EINTR;
		}

		goto retry;
	    }
	    if (cmd != 65)  /*  not probe read   */
		    printk ("xfd: error sta=%x FDC error (STATE_CMD)"
			    "cyl=%d head=%d sector=%d\n",
			    v->x0, v->xa, v->xb, v->xc);
	}

	restore_flags (flags);

	return xfd_cmd_ret;
}


#ifndef SIOCFORMAT
/*  compatibility with SVR3.1   */

#define SIOCBD00        (('s'<<8)|1)    /* ioctl: set badblk valid,but none */
#define SIOCBASS        (('s'<<8)|2)    /* ioctl: ass. badtracks        */
#define SIOCFORMAT      (('s'<<8)|3)    /* ioctl: format                */
#define SIOCBDBK        (('s'<<8)|4)    /* ioctl: set badblock illegal  */
#define SIOCSETP        (('s'<<8)|5)    /* ioctl: set testmode          */
#define SIOCGETP        (('s'<<8)|6)    /* ioctl: get volume-infos      */
#define SIOCTRSPMOD     (('s'<<8)|18)   /* ioctl: transparent mode      */
#endif  /*  SIOCFORMAT   */

struct dkinfo {                         /* for SIOCGETP         */
    struct dkvol {
	ushort  unit;                   /* select info                  */
	ushort  disktype;               /* disk specific type           */
	ushort  sec_p_track;            /* sectors / track              */
	ushort  hd_offset;              /* add this to head no 0        */
	ushort  hd_p_vol;               /* heads per volume             */
	ushort  cyl_p_vol;              /* cylinders per volume         */
	char    steprate;               /* steprate seek                */
				/* following infos for format only :    */
	char    interleave;             /* hardwared sector interleave  */
	char    bias_trtr;              /* serpentine track to track    */
	char    bias_hdhd;              /* serpentine next cylinder     */
    } vol;
    struct dkldev {
	uint    bl_offset;              /* disk block offset            */
	uint    bl_size;                /* total no of blocks on minor  */
    } ldev;
};

static int xfd_ioctl (struct inode *inode, struct file *file,
		      unsigned int cmd, unsigned long arg) {
	int dev, err;

	dev = MINOR(inode->i_rdev);
	if (dev == 0)  dev = xfd_real_dev;      /*  autoprobed dev   */

	switch (cmd) {

	    case BLKGETSIZE:   /* Return device size */
		if (!arg)  return -EINVAL;
		err = verify_area (VERIFY_WRITE, (long *) arg, sizeof(long));
		if (err)
			return err;
		put_fs_long (xfd_part[dev].nr_sects << 1, (long *) arg);
		return 0;

	    case SIOCFORMAT:  /*  format diskette (svr3)  */
		err = do_xfd_format (dev);
		return err;

	    case SIOCGETP:    /*  get parameters (svr3)  */
		{   struct dkinfo dkinfo;

		    err = verify_area (VERIFY_WRITE, (void *) arg,
							sizeof (dkinfo));
		    if (err)  return err;

		    dkinfo.vol.unit = 0;
		    dkinfo.vol.disktype = dev;
		    dkinfo.vol.sec_p_track = xfdpar[dev].f_sptrack;
		    dkinfo.vol.hd_offset = 0;
		    dkinfo.vol.hd_p_vol = xfdpar[dev].f_heads;
		    dkinfo.vol.cyl_p_vol = xfdpar[dev].f_cyl;
		    dkinfo.vol.steprate = xfdpar[dev].f_steprate;
		    dkinfo.vol.interleave = xfdpar[dev].f_interleave;
		    dkinfo.vol.bias_trtr = 0;
		    dkinfo.vol.bias_hdhd = 0;
		    dkinfo.ldev.bl_offset = xfd_part[dev].start_sect;
		    dkinfo.ldev.bl_size = xfd_part[dev].nr_sects;

		    memcpy_tofs ((void *) arg, &dkinfo, sizeof (dkinfo));
		}
		break;

	    case SIOCBD00:      /*  svr3   */
	    case SIOCBDBK:      /*  svr3   */
	    case SIOCSETP:      /*  svr3   */
		break;

	    default:
		return -EINVAL;
		break;
	}

	return 0;
}

static int do_xfd_format (int dev) {
	volatile struct xfd *v = (struct xfd *) XFD_ADDR;
	unsigned short flags;
	int sectors = xfdpar[dev].f_sptrack;
	int i,n,m;

	if (dev == 0)  return -ENODEV;  /*  autoprobing dev  */

	for (i=0, n=0, m=0; i < sectors && i < 32;i++) {
	    xfdinterl[i] = n + xfdpar[dev].f_firstsec;
	    n += xfdpar[dev].f_interleave;
	    if (n >= sectors) n = m++;
	    else  n++;
	}

	save_flags (flags);
	cli();

	if (xfd_busy) {
		restore_flags (flags);
		return  -EBUSY;
	}
	xfd_busy = STATE_FORMAT;

	v->x1 = (xfdpar[dev].f_cyl == 80 ? 0 : 128) | xfdpar[dev].f_bps;
	v->x8 = xfdpar[dev].f_dens;
	v->x9 = 1;  /*  but may be 0 in some odd cases ???   */

	xfd_retry = 0;

	v->x4 = &xfdinterl;
	v->xc = xfdpar[dev].f_sptrack;
	v->xa = 0;
	v->xb = 0;

	v->x0 = 68;

	xfd_real_dev = dev;

	interruptible_sleep_on (&cmd_wait);
	if (xfd_busy != STATE_FREE) {
		xfd_busy = STATE_BREAK;
		restore_flags (flags);
		return -EINTR;
	}

	restore_flags (flags);

	return xfd_cmd_ret;
}

static void end_request (int uptodate, int major) {
	struct request *curr = blk_dev[major].current_request;
	struct buffer_head *bh;

	curr->errors = 0;

	if (!uptodate) {
	    printk("end_request: I/O error, dev %04lX, sector %lu\n",
			(unsigned long) curr->rq_dev, curr->sector >> 1);

	    curr->nr_sectors--;
	    curr->nr_sectors &= ~SECTOR_MASK;
	    curr->sector += (BLOCK_SIZE/512);
	    curr->sector &= ~SECTOR_MASK;
	}
	else if (curr->cmd == READ)
		/*  because `bh->b_data area' was fulled by DMA   */
		clear_data_cache (curr->buffer, curr->current_nr_sectors << 9);

	if((bh=curr->bh) != NULL) {

	    curr->bh = bh->b_reqnext;
	    bh->b_reqnext = NULL;

	    mark_buffer_uptodate(bh, uptodate);
	    unlock_buffer (bh);

	    if ((bh = curr->bh) != NULL) {
		curr->current_nr_sectors = bh->b_size >> 9;
		if (curr->nr_sectors < curr->current_nr_sectors) {
		    curr->nr_sectors = curr->current_nr_sectors;
		    printk ("end_request: buffer list destroyed\n");
		}
		curr->buffer = bh->b_data;
		return;
	    }
	}

	blk_dev[major].current_request = curr->next;
	if (curr->sem != NULL)
		up (curr->sem);
	curr->rq_status = RQ_INACTIVE;

	wake_up (&wait_for_request);

	return;
}

