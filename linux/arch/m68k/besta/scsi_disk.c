/*
 * besta/scsi_disk.c -- SCSI-disk driver.
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
#include <linux/malloc.h>

#include <linux/interrupt.h>

#include <asm/setup.h>
#include <asm/pgtable.h>
#include <asm/system.h>
#include <asm/segment.h>

#include "besta.h"
#include "scsi.h"

static int scsi_disk_open (struct inode *inode, struct file *filp);
static void scsi_disk_release (struct inode *inode, struct file *file);
static void scsi_disk_inthandler (int target, int err,
					struct scsi_info_struct *scsi_info);
static int scsi_disk_ioctl (struct inode *inode, struct file *filp,
				      unsigned int cmd, unsigned long arg);
static int scsi_get_partitions (int target, char *buf,
					struct hd_struct *part, int *);
static int scsi_default_partition (int size, int blksize,
					    struct hd_struct *part);

static struct file_operations scsi_disk_fops = {
	NULL,                   /* lseek - default */
	block_read,             /* read - general block-dev read */
	block_write,            /* write - general block-dev write */
	NULL,                   /* readdir - bad */
	NULL,                   /* select */
	scsi_disk_ioctl,        /* ioctl */
	NULL,                   /* mmap */
	scsi_disk_open,         /* open */
	scsi_disk_release,      /* release */
	block_fsync,            /* fsync */
	NULL,                   /* fasync */
	NULL,                   /* media_change */
	NULL,                   /* revalidate */
};

extern unsigned long __do_scsi_request_base;    /*  really it is static   */
static void do_scsi_request (int major);

void scsi_disk_init (int target, char *inquiry_buffer,
				    struct scsi_info_struct *scsi_info) {
	int blksize, major, j, r_part;
	struct {
	    int size;
	    int blksize;
	} cap;          /*  buffer for read capacity   */
	char *buf;      /*  for sector 0  (partitions)  */
	char read_6_cmd[12] = { READ_6, 0,0,0,0,0,0,0,0,0,0,0 };

	/*  spin up stuff...   */
	if (scsi_info[target].state == STATE_NOT_READY) {
	    int res;
	    int sec_wait = 16;  /*  seconds wait for spin up successful...  */

	    printk ("(spinning up, wait for %d seconds...)", sec_wait);

	    res = do_cmd_in_init (target, scsi_start_stop, 0, 0, 0,
							    sec_wait * HZ);

	    if (res) {
		printk ("not responding...)");
		scsi_info[target].type = TYPE_NONE;
		return;
	    }

	    printk ("OK)");
	    scsi_info[target].state = STATE_FREE;
	}


	/*  read capacity stuff...  */
	for (j = 3; j > 0; j--)
	    if (!do_cmd_in_init (target, scsi_read_capacity, 0, &cap, 8, 0))
		    break;

	if (j == 0) {
	    printk ("-- CANNOT READ CAPACITY !");
	    scsi_info[target].type = TYPE_NONE;
	    return;
	}
	cap.size += 1;      /*  num of blocks == last avail + 1   */

	disk_info(target).size = cap.size;
	scsi_info[target].blksize = cap.blksize;

	printk ("%dMb (%dk), sector=%db", (cap.size*cap.blksize)>>20,
			    (cap.size*cap.blksize)>>10, cap.blksize);


	/*  mode  sense / select  stuff   */

	scsi_disk_mode (target, scsi_info);

	blksize = scsi_info[target].blksize;    /*  only hear valid  */

	if (blksize != 1024 &&
	    blksize != 512 &&
	    blksize != 256
	) {
	    printk ("-- WRONG BLKSIZE !");
	    scsi_info[target].type = TYPE_NONE;
	    return;
	}


	/*  partition stuff...   */
	buf = (char *) kmalloc (1024, GFP_KERNEL);
	disk_info(target).part =
		kmalloc (MAX_PARTS * sizeof (struct hd_struct), GFP_KERNEL);

	read_6_cmd[4] = 1024 / scsi_info[target].blksize;

	r_part = 0;     /*  root partition number   */

	if (do_cmd_in_init (target, read_6_cmd, 0, buf, 1024, 0) ||
	    !scsi_get_partitions (target, buf, disk_info(target).part, &r_part)
	)  {
	    printk (" (default partitions)");
	    scsi_default_partition (cap.size, cap.blksize,
						disk_info(target).part);
	}

	kfree (buf);


	major = scsi_info[target].major;

	/*  If ROOT_DEV is yet not set and some of partitions marked as root,
	   let it partition to be a root device...
	*/
	if (!ROOT_DEV && r_part)
		ROOT_DEV = to_kdev_t (MKDEV (major, r_part - 1));

	if (register_blkdev (major, name(target), &scsi_disk_fops)) {
		printk ("Unable to get major %d ",major);
		scsi_info[target].type = TYPE_NONE;
		return;
	}

	scsi_info[target].inthandler = scsi_disk_inthandler;

	/*  `request_fn' is called without any arguments, but we want
	  to send our `do_request' function a target index. It is
	  needable, because we use different major numbers for different
	  SCSI targets/controllers. The easy way to get target is to use
	  `scsi_targets' array with `target <--> major' dependence.
	  But for it we should send to `request_fn' function the major
	  number.
	    Because `request_fn' hasn`t arguments, we use an array of
	  function, `__do_scsi_request_base', which by assemble way
	  translate the control to `do_scsi_request' function with the
	  right major number. Ugly, ugly, ugly...
	*/
	blk_dev[major].request_fn = (void *) (&__do_scsi_request_base + major);
	if (0)  j = (int) &do_scsi_request;     /*  to make gcc happy   */


	read_ahead[major] = 0;
	blksize_size[major] = (int *) kmalloc (MAX_PARTS * sizeof (int),
								GFP_KERNEL);
	if (blksize_size[major])
		for (j=0;j < MAX_PARTS;j++)  blksize_size[major][j] = blksize;


	return;
}


static int scsi_disk_open (struct inode *inode, struct file *filp) {
	int minor = MINOR (inode->i_rdev);
	int major = MAJOR (inode->i_rdev);
	int target;
	struct scsi_info_struct *scsi_info;

	if (minor >= MAX_PARTS)  return -ENODEV;

	/* find a target for this major  */
	if (scsi_targets[major] < 0)  return -ENODEV;
	target = scsi_targets[major] & 0x7;
	scsi_info = scsi_infos[scsi_targets[major] >> 3];

	if (scsi_info[target].state == STATE_REVALIDATE)  return -EBUSY;

	scsi_info[target].access_cnt++;

	if (filp && filp->f_mode)
	    check_disk_change (inode->i_rdev);

	return 0;
}

static void scsi_disk_release (struct inode *inode, struct file *file) {
	int target;
	int major = MAJOR (inode->i_rdev);
	struct scsi_info_struct *scsi_info;

	/* find a target for this major  */
	if (scsi_targets[major] < 0)  return;
	target = scsi_targets[major] & 0x7;
	scsi_info = scsi_infos[scsi_targets[major] >> 3];

	scsi_info[target].access_cnt--;

	sync_dev (inode->i_rdev);

	return;
}


static void scsi_disk_inthandler (int target, int err,
					struct scsi_info_struct *scsi_info) {
	int major = scsi_info[target].major;
	int board = scsi_info[target].board;
	struct request *curr;

	if (scsi_info[target].req_sense) {      /*  was check conditions   */

	    scsi_info[target].req_sense = 0;

	    scsi_print_sense (name (target), scsi_info[target].sense_buf);

	    if (scsi_info[target].state == STATE_IO) {

		scsi_info[target].end_request (0, board, target, major);

		scsi_info[target].state = STATE_FREE;
		if (waitqueue_active (&scsi_info[target].wait))
			wake_up (&scsi_info[target].wait);

	    } else {

		scsi_info[target].cmd_ret = -EIO;

		scsi_info[target].state = STATE_FREE;
		wake_up (&scsi_info[target].wait);
	    }

	    /*  always run request queue...  */
	    scsi_info[target].do_request (board, target, major);

	    return;
	}


	if (scsi_info[target].state == STATE_IO) {

	    curr = blk_dev[major].current_request;

	    if (!err) {     /*  OK  */
		curr->nr_sectors -= curr->current_nr_sectors;
		curr->sector += curr->current_nr_sectors;

		scsi_info[target].end_request (1, board, target, major);

	    } else      /*  bad  */
		scsi_info[target].end_request (0, board, target, major);

	    scsi_info[target].state = STATE_FREE;
	    if (waitqueue_active (&scsi_info[target].wait))
		    wake_up (&scsi_info[target].wait);

	}
	else if (scsi_info[target].state == STATE_CMD) {

	    if (!err)       /* OK  */
		    scsi_info[target].cmd_ret = 0;
	    else
		    scsi_info[target].cmd_ret = -EIO;

	    scsi_info[target].state = STATE_FREE;
	    wake_up (&scsi_info[target].wait);
	}

	/*  always run request queue...  */
	scsi_info[target].do_request (board, target, major);

	return;
}


static int scsi_disk_ioctl (struct inode *inode, struct file *filp,
				      unsigned int cmd, unsigned long arg) {
	int target, err;
	int minor = MINOR (inode->i_rdev);
	int major = MAJOR (inode->i_rdev);
	struct scsi_info_struct *scsi_info;

	/* find a target for this major  */
	if (scsi_targets[major] < 0)  return -ENODEV;
	target = scsi_targets[major] & 0x7;
	scsi_info = scsi_infos[scsi_targets[major] >> 3];


	minor = MINOR (inode->i_rdev);

	switch (cmd) {

	    case BLKGETSIZE:   /* Return device size */
		if (!arg)  return -EINVAL;
		err = verify_area (VERIFY_WRITE, (long *) arg, sizeof(long));
		if (err)
			return err;
		put_fs_long (disk_info(target).part[minor].nr_sects << 1,
							    (long *) arg);
		return 0;
		break;

	    case BLKRASET:
		if (!suser())  return -EACCES;
		if (!(inode->i_rdev)) return -EINVAL;
		if(arg > 0xff) return -EINVAL;

		read_ahead[MAJOR(inode->i_rdev)] = arg;

		return 0;
		break;

	    case BLKRAGET:
		if (!arg)  return -EINVAL;
		err = verify_area(VERIFY_WRITE, (int *) arg, sizeof(int));
		if (err)  return err;

		put_user (read_ahead[MAJOR (inode->i_rdev)], (int *) arg);

		return 0;
		break;

	    case BLKFLSBUF:

		if(!suser())  return -EACCES;
		if(!inode->i_rdev) return -EINVAL;

		fsync_dev(inode->i_rdev);
		invalidate_buffers(inode->i_rdev);

		return 0;
		break;

	    case BLKRRPART:     /* Re-read partition tables */
		{   char *buf;
		    int i;
		    struct hd_struct *new_part;
		    unsigned short flags;
		    char read_6_cmd[12] = { READ_6, 0,0,0,0,0,0,0,0,0,0,0 };

		    buf = (char *) kmalloc (1024, GFP_KERNEL);
		    if (!buf)  return -ENOMEM;

		    read_6_cmd[4] = 1024 / scsi_info[target].blksize;

		    err = do_scsi_cmd (target, read_6_cmd, 0, buf, 1024);
		    if (err)  { kfree (buf);  return err; }

		    new_part = (struct hd_struct *)
			    kmalloc (MAX_PARTS * sizeof (struct hd_struct),
								GFP_KERNEL);
		    if (!new_part)  { kfree (buf);  return -ENOMEM; }

		    if (!scsi_get_partitions (target, buf, new_part, 0)) {
			    kfree (buf);
			    kfree (new_part);
			    return -EINVAL;
		    }

		    kfree (buf);

		    /*  not busy  and  only one device open (for ioctl)  */
		    save_flags (flags);
		    cli();
		    if (scsi_info[target].state != STATE_FREE ||
			scsi_info[target].access_cnt > 1
		    ) {
			restore_flags (flags);
			kfree (new_part);
			return -EBUSY;
		    }
		    scsi_info[target].state = STATE_REVALIDATE;
		    restore_flags (flags);

		    for (i = 0; i < MAX_PARTS; i++) {
			int dev;

			if (!disk_info(target).part[i].nr_sects)  continue;

			dev = MKDEV (MAJOR (inode->i_rdev), i);
			sync_dev (dev);
			invalidate_inodes (dev);
			invalidate_buffers (dev);
		    }

		    kfree (disk_info(target).part);
		    disk_info(target).part = new_part;

		    scsi_info[target].state = STATE_FREE;

		    return 0;
		}
		break;

	    case HDIO_GETGEO:   /* Return BIOS disk parameters */
		{   struct hd_geometry hd_geom;

		    if (!arg)  return -EINVAL;
		    err = verify_area (VERIFY_WRITE, (void *) arg,
							sizeof (hd_geom));
		    if (err)  return err;

		    hd_geom.heads = disk_info(target).heads;
		    hd_geom.sectors = disk_info(target).sects_per_track;
		    hd_geom.cylinders = disk_info(target).cylinders;
		    hd_geom.start = disk_info(target).part[minor].start_sect;

		    memcpy_tofs ((void *) arg, &hd_geom, sizeof (hd_geom));

		    return 0;
		}
		break;

	    case SIOCBD00:      /*  svr3   */
	    case SIOCBDBK:      /*  svr3   */
	    case SIOCSETP:      /*  svr3   */
		break;

	    case SIOCFORMAT:
		return -ENOSYS;     /* while unimplemented  */
		break;

	    case SIOCGETP:      /*  get parameters (svr3)  */
		{ struct dkinfo {             /* for SIOCGETP         */
		      struct dkvol {
			  ushort  unit;       /* select info                 */
			  ushort  disktype;   /* disk specific type          */
			  ushort  sec_p_track;/* sectors / track             */
			  ushort  hd_offset;  /* add this to head no 0       */
			  ushort  hd_p_vol;   /* heads per volume            */
			  ushort  cyl_p_vol;  /* cylinders per volume        */
			  char    steprate;   /* steprate seek               */
				      /* following infos for format only :   */
			  char    interleave; /* hardwared sector interleave */
			  char    bias_trtr;  /* serpentine track to track   */
			  char    bias_hdhd;  /* serpentine next cylinder    */
		      } vol;
		      struct dkldev {
			  uint    bl_offset;  /* disk block offset           */
			  uint    bl_size;    /* total no of blocks on minor */
		      } ldev;
		  } dkinfo;

		  err = verify_area (VERIFY_WRITE, (void *) arg,
							sizeof (dkinfo));
		  if (err)  return err;

		  if (scsi_info[target].type != TYPE_DISK)  return -ENOTBLK;

		  dkinfo.vol.unit = target;
		  dkinfo.vol.disktype = scsi_info[target].type;
		  dkinfo.vol.sec_p_track = disk_info(target).sects_per_track;
		  dkinfo.vol.hd_offset = 0;
		  dkinfo.vol.hd_p_vol = disk_info(target).heads;
		  dkinfo.vol.cyl_p_vol = disk_info(target).cylinders;
		  dkinfo.vol.steprate = 0;
		  dkinfo.vol.interleave = 0;
		  dkinfo.vol.bias_trtr = 0;
		  dkinfo.vol.bias_hdhd = 0;
		  dkinfo.ldev.bl_offset =
				disk_info(target).part[minor].start_sect;
		  dkinfo.ldev.bl_size = disk_info(target).part[minor].nr_sects;

		  memcpy_tofs ((void *) arg, &dkinfo, sizeof (dkinfo));
		}

		return 0;
		break;

	    default:
		return scsi_ioctl (target, scsi_info, filp, cmd, arg);
		break;
	}

	return 0;
}


static int scsi_get_partitions (int target, char *buf,
				struct hd_struct *part, int *root_index_ptr) {
	struct hard_disk_partition *hdpp = (struct hard_disk_partition *) buf;
	unsigned int *lp, checksum = 0;
	int i, area_to_check;

	if (hdpp->magic != HDP_MAGIC ||
	    hdpp->num_parts == 0 ||
	    hdpp->num_parts > sizeof (hdpp->parts) / sizeof (hdpp->parts[0])
	)  return 0;

	/*  compute a check sum...  */
	area_to_check = hdpp->num_parts * sizeof (struct hd_partition) +
			(((int) &((struct hard_disk_partition *) 0)->parts) -
			 ((int) &((struct hard_disk_partition *) 0)->magic));
	area_to_check /= sizeof (*lp);

	lp = &hdpp->magic;
	for (i = 0; i < area_to_check; i++)  checksum ^= *lp++;

	/*  compare with stored check sum...  */
	checksum ^= *lp;

	if (checksum)  return 0;    /*  should be zero   */

	if (hdpp->num_parts > MAX_PARTS) {
		printk (" (Too many partitions %d . Trancate for %d)",
					    hdpp->num_parts, MAX_PARTS);
		hdpp->num_parts = MAX_PARTS;
	}

	/*  OK, initialize partitions.  */
	for (i = 0; i < hdpp->num_parts; i++) {
		part[i].start_sect = hdpp->parts[i].start_sect;
		part[i].nr_sects = hdpp->parts[i].nr_sects;
	}
	for ( ; i < MAX_PARTS; i++) {
		part[i].start_sect = 0;
		part[i].nr_sects = 0;
	}

	if (root_index_ptr &&
	    hdpp->root_part != 0 &&
	    hdpp->root_part <= hdpp->num_parts
	)  *root_index_ptr = hdpp->root_part;

	return 1;
}


/*  This kludge is for disks which yet do not support partition table
   at "boot" sector. Emulate old SVR3 kern behavior in the vendor style...
*/
static int scsi_default_partition (int size, int blksize,
					    struct hd_struct *part) {
	int capacity = (size * blksize) >> 10;  /*  in kilobytes   */
	int i;

#define HIGH_BOUNDARY   400000
#define LOW_BOUNDARY    100000

	if (capacity > HIGH_BOUNDARY) {

	    capacity -= (capacity % 10000);

	    part[0].start_sect = 0;
		part[0].nr_sects = 250000;
	    part[1].start_sect = 265000;
		part[1].nr_sects = 35000;
	    part[2].start_sect = 250000;
		part[2].nr_sects = 15000;
	    part[3].start_sect = capacity;
		part[3].nr_sects = 900000;
	    part[4].start_sect = 0;
		part[4].nr_sects = 0;
	    part[5].start_sect = 300000;
		part[5].nr_sects = capacity - 300000;
	    part[6].start_sect = 0;
		part[6].nr_sects = 300000;
	    part[7].start_sect = 0;
		part[7].nr_sects = capacity;
	}
	else if (capacity > LOW_BOUNDARY) {

	    capacity -= (capacity % 4000);

	    part[0].start_sect = 0;
		part[0].nr_sects = capacity - 50000;
	    part[1].start_sect = capacity - 35000;
		part[1].nr_sects = 35000;
	    part[2].start_sect = capacity - 50000;
		part[2].nr_sects = 15000;
	    part[3].start_sect = capacity;
		part[3].nr_sects = 900000;
	    part[4].start_sect = 0;
		part[4].nr_sects = 0;
	    part[5].start_sect = 0;
		part[5].nr_sects = 0;
	    part[6].start_sect = 0;
		part[6].nr_sects = capacity;
	    part[7].start_sect = 0;
		part[7].nr_sects = capacity;
	}
	else {

	    part[0].start_sect = 0;
		part[0].nr_sects = capacity;
	    part[1].start_sect = 0;
		part[1].nr_sects = 0;
	    part[2].start_sect = 0;
		part[2].nr_sects = 0;
	    part[3].start_sect = capacity;
		part[3].nr_sects = 900000;
	    part[4].start_sect = 0;
		part[4].nr_sects = 0;
	    part[5].start_sect = 0;
		part[5].nr_sects = 0;
	    part[6].start_sect = 0;
		part[6].nr_sects = 0;
	    part[7].start_sect = 0;
		part[7].nr_sects = capacity;
	}

	for (i = 8; i < MAX_PARTS; i++) {
		part[i].start_sect = 0;
		part[i].nr_sects = 0;
	}

	return 0;
}


/*  The hackish way to send our `request_fn'-clone function a major number   */

__asm ("
__do_scsi_request_base:
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	bsr.w   1f
	nop

1:
	mov.l   (%sp),%d0
	sub.l   &__do_scsi_request_base + 4,%d0
	lsr.l   &2,%d0
	mov.l   %d0,(%sp)
	jsr     do_scsi_request
	add.l   &4,%sp
	rts" );

static void do_scsi_request (int major) {
	int target;
	struct scsi_info_struct *scsi_info;

	/* find a target for this major  */
	if (scsi_targets[major] < 0)  return;
	target = scsi_targets[major] & 0x7;
	scsi_info = scsi_infos[scsi_targets[major] >> 3];

	scsi_info[target].do_request (scsi_info[target].board, target, major);

	return;
}
