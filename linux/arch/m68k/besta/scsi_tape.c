/*
 * besta/scsi_tape.c -- SCSI-tape driver.
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

static int scsi_tape_open (struct inode *inode, struct file *filp);
static void scsi_tape_release (struct inode *inode, struct file *file);
static int scsi_tape_read (struct inode *inode, struct file *filp,
						char * buf, int count);
static int scsi_tape_write (struct inode *inode, struct file *filp,
					    const char * buf, int count);
static void scsi_tape_inthandler (int target, int err,
					struct scsi_info_struct *scsi_info);
static int scsi_tape_ioctl (struct inode *inode, struct file *filp,
				      unsigned int cmd, unsigned long arg);

static struct file_operations scsi_tape_fops = {
	NULL,                   /* lseek - default */
	scsi_tape_read,         /* read - general block-dev read */
	scsi_tape_write,        /* write - general block-dev write */
	NULL,                   /* readdir - bad */
	NULL,                   /* select */
	scsi_tape_ioctl,        /* ioctl */
	NULL,                   /* mmap */
	scsi_tape_open,         /* open */
	scsi_tape_release,      /* release */
	NULL,                   /* fsync */
	NULL,                   /* fasync */
	NULL,                   /* media_change */
	NULL,                   /* revalidate */
};

static struct tape_config {
	char    vendor[8];
	char    model[16];
	int     tmslen;
	char    tmsdata[64];
} tape_config[] = {
    {   "TANDBERG", " TDC 3600",
	15,
	{ 0, 0, 0x10, 0xb, 0x10, 0, 0, 0, 0, 0, 0x2, 0, 0, 0xa, 0xa, 0, }
    }
};

#define MAX_XTAPE_CONFIG  (sizeof(tape_config)/sizeof(struct tape_config))


extern void scsi_tape_init (int target, char *inquiry_buffer,
					struct scsi_info_struct *scsi_info) {
	int conf, major;
	char mode_select[] = scsi_mode_select;

	for (conf = 0; conf < MAX_XTAPE_CONFIG; conf++)
	    if (strncmp (tape_config[conf].vendor,
			 &inquiry_buffer[8],
			 strlen (tape_config[conf].vendor)) == 0 &&
		strncmp (tape_config[conf].model,
			 &inquiry_buffer[16],
			 strlen (tape_config[conf].model)) == 0
	    )  break;
	if (conf == MAX_XTAPE_CONFIG) {
	    printk ("(currently unsupported)");
	    scsi_info[target].type = TYPE_NONE;
	    return;
	}

	mode_select[4] = tape_config[conf].tmslen;
	if (do_cmd_in_init (target, mode_select, 1,
		    tape_config[conf].tmsdata, tape_config[conf].tmslen, 0)) {
	    printk ("-- BAD TMSELECT !");
	    scsi_info[target].type = TYPE_NONE;
	    return;
	}

	scsi_info[target].blksize = 512;

	printk (", block=512b");

	major = scsi_info[target].major;
	scsi_info[target].inthandler = scsi_tape_inthandler;

	if (register_chrdev (major, name(target), &scsi_tape_fops)) {
		printk ("Unable to get major %d ",major);
		scsi_info[target].type = TYPE_NONE;
		return;
	}

	return;
}


static int scsi_tape_open (struct inode *inode, struct file *filp) {
	int target, err;
	struct scsi_info_struct *scsi_info;
	unsigned short flags;
	int major = MAJOR (inode->i_rdev);

	/* find a target for this major  */
	if (scsi_targets[major] < 0)  return -ENODEV;
	target = scsi_targets[major] & 0x7;
	scsi_info = scsi_infos[scsi_targets[major] >> 3];


	save_flags (flags);
	cli();
	if (scsi_info[target].access_cnt) {
		restore_flags (flags);
		return -EBUSY;
	}
	scsi_info[target].access_cnt = 1;
	restore_flags (flags);

	tape_info(target).written = 0;  /* Tape is written, if 1  */
	tape_info(target).ended = 0;    /* Tape is vasya, if 1  */

	scsi_info[target].buffer = kmalloc (512, GFP_KERNEL);
	if (!scsi_info[target].buffer)  { err = -ENOMEM;  goto fail_open; }

	/*  Trsp disallow  removal   */
	err = do_scsi_cmd (target, scsi_disall_md_removal, 1, 0, 0);
	if (err)  goto fail_open;

	if ((MINOR(inode->i_rdev) & 0x7) != 0) {
	    /* rewind  */
	    err = do_scsi_cmd (target, scsi_rezero_unit, 1, 0, 0);
	    if (err)  goto fail_open;
	}

	return 0;

fail_open:
	scsi_info[target].access_cnt = 0;

	return err;
}


static void scsi_tape_release (struct inode *inode, struct file *file) {
	int target;
	struct scsi_info_struct *scsi_info;
	int major = MAJOR (inode->i_rdev);

	/* find a target for this major  */
	if (scsi_targets[major] < 0)  return;
	target = scsi_targets[major] & 0x7;
	scsi_info = scsi_infos[scsi_targets[major] >> 3];


	/*  Write filemark, if it is needable   */
	if (tape_info(target).written)
	    do_scsi_cmd (target, scsi_write_filemark, 1, 0, 0);

	/* rewind  */
	if ((MINOR(inode->i_rdev) & 0x7) > 1)
	    do_scsi_cmd (target, scsi_rezero_unit, 1, 0, 0);

	tape_info(target).written = 0;
	tape_info(target).ended = 0;

	/*  Trsp allow removal   */
	do_scsi_cmd (target, scsi_all_md_removal, 1, 0, 0);

	kfree (scsi_info[target].buffer);

	scsi_info[target].access_cnt = 0;

	return;
}

static int scsi_tape_write (struct inode *inode, struct file *filp,
					    const char * buf, int count) {
	int target;
	struct scsi_info_struct *scsi_info;
	int major = MAJOR (inode->i_rdev);
	int blocks, err;
	int total = 0;
	unsigned char write_6_cmd[12] = { WRITE_6, 1, 0, 0, 1, 0, };

	/* only allow mod 512 bytes at a time */
	if (count % 512 != 0)  return -EIO;
	blocks = count >> 9;

	/* Make sure buffer is safe to read from. */
	err = verify_area (VERIFY_READ, buf, count);
	if (err)  return err;

	/* find a target for this major  */
	if (scsi_targets[major] < 0)  return -ENODEV;
	target = scsi_targets[major] & 0x7;
	scsi_info = scsi_infos[scsi_targets[major] >> 3];


	/*  currently write in transparent mode per simple block...  */
	while (blocks--) {

	    memcpy_fromfs (scsi_info[target].buffer, (void *) buf, 512);

	    err = do_scsi_cmd (target, write_6_cmd, 1,
					scsi_info[target].buffer, 512);

	    if (err)  return err;

	    tape_info(target).written = 1;
	    tape_info(target).ended = 0;

	    total += 512;
	    buf += 512;
	}

	return total;
}

static int scsi_tape_read (struct inode *inode, struct file *filp,
						char * buf, int count) {
	int target;
	struct scsi_info_struct *scsi_info;
	int major = MAJOR (inode->i_rdev);
	int blocks, err;
	int total = 0;
	unsigned char read_6_cmd[12] = { READ_6, 1, 0, 0, 1, 0, };

	/* only allow mod 512 bytes at a time */
	if (count % 512 != 0)  return -EIO;
	blocks = count >> 9;

	/* Make sure buffer is safe to write into. */
	err = verify_area (VERIFY_WRITE, buf, count);
	if (err)  return err;

	/* find a target for this major  */
	if (scsi_targets[major] < 0)  return -ENODEV;
	target = scsi_targets[major] & 0x7;
	scsi_info = scsi_infos[scsi_targets[major] >> 3];


	if (tape_info(target).ended)  return 0;

	while (blocks--) {

	    err = do_scsi_cmd (target, read_6_cmd, 0,
					scsi_info[target].buffer, 512);
	    if (err)  return err;

	    memcpy_tofs((void *) buf, scsi_info[target].buffer, 512);

	    total += 512;
	    buf += 512;

	    if (tape_info(target).ended) {
		total -= tape_info(target).resid;
		break;
	    }
	}

	return total;
}


static void scsi_tape_inthandler (int target, int err,
					struct scsi_info_struct *scsi_info) {

	if (scsi_info[target].req_sense) {  /*  was check conditions   */
	    unsigned char *sense_buf = scsi_info[target].sense_buf;

	    scsi_info[target].req_sense = 0;

	    /*  most common case   */
	    scsi_info[target].cmd_ret = -EIO;

	    if ((sense_buf[0] & 0x70) == 0x70) {  /*  extended sense data  */
		switch (sense_buf[2] & 0xf) {

		    case NO_SENSE:
		    case RECOVERED_ERROR:
			if (sense_buf[2] & 0x20)  break;  /* ILI  */

			/*  else  FM or EOM detected   */

		    case BLANK_CHECK:
			if (sense_buf[0] & 0x80)
			    tape_info(target).resid =
				*((unsigned long *) &sense_buf[3]) *
						scsi_info[target].blksize;
			else  tape_info(target).resid = 0;
			tape_info(target).ended = 1;

			scsi_info[target].cmd_ret = 0;
			break;

		    case UNIT_ATTENTION:
			scsi_info[target].cmd_ret = 0;
			break;

		    case DATA_PROTECT:
			scsi_info[target].cmd_ret = -EROFS;
			break;

		    case VOLUME_OVERFLOW:
			scsi_info[target].cmd_ret = -ENOSPC;
			break;

		    default:
			break;
		}
	    }

	    if (scsi_info[target].cmd_ret)
		    scsi_print_sense (name (target), sense_buf);
	}
	else {

	    if (!err)
		    scsi_info[target].cmd_ret = 0;
	    else
		    scsi_info[target].cmd_ret = -EIO;
	}

	scsi_info[target].state = STATE_FREE;
	wake_up (&scsi_info[target].wait);

	return;
}


static int scsi_tape_ioctl (struct inode *inode, struct file *filp,
				      unsigned int cmd, unsigned long arg) {
	int target;
	struct scsi_info_struct *scsi_info;
	int major = MAJOR (inode->i_rdev);

	/* find a target for this major  */
	if (scsi_targets[major] < 0)  return -ENODEV;
	target = scsi_targets[major] & 0x7;
	scsi_info = scsi_infos[scsi_targets[major] >> 3];

	/*  currently nothing special   */
	return scsi_ioctl (target, scsi_info, filp, cmd, arg);
}
