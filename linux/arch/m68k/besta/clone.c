/*
 * besta/clone.c -- Emulation of System V clone driver.
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

extern struct file_operations *get_chrfops (unsigned int major,
						unsigned int minor);

static int clone_open (struct inode *inode, struct file *filp);

static struct file_operations clone_fops = {
	NULL,
	NULL,       /*  no read   */
	NULL,       /*  no write  */
	NULL,
	NULL,
	NULL,       /*  no ioctl   */
	NULL,
	clone_open,
	NULL,       /*  no release   */
	NULL,
	NULL,
	NULL,
	NULL,
};

void clone_init (const char *name, int major) {

	if (register_chrdev (major, name, &clone_fops))
	    printk ("Unable to get major %d\n", major);

	return;
}

/*  Opening  a clone device (CLONE_MAJOR,CLONE_MINOR)  really gets
  a first unused minor (channel)  of  major == CLONE_MINOR  device and
  overload itself by opening (CLONE_MINOR,generated_minor) device.
    Under sysv this is useful only for `stream' devices,
  but hear it is easy to implement this for all devs (is it good idea???)
*/

static int clone_open (struct inode *inode, struct file *file) {
	int clone_minor = MINOR (inode->i_rdev);
	struct file_operations *fops;
	struct inode *new_inode, *tmp;
	char minor_busy[256];
	int i;
	unsigned short flags;

	/*  clone minor becames a device major   */
	fops = get_chrfops (clone_minor, 0);
	if (!fops)  return -ENXIO;

	/*  make a new inode   */
	new_inode = get_empty_inode();
	if (!new_inode)  return -ENOMEM;

	/*  get minimal unused minor number   */
	memset (minor_busy, 0, 256);
	new_inode->i_rdev = 0;

	save_flags (flags);
	cli ();

	tmp = inode->i_next;
	for (i = nr_inodes; i > 0; i--, tmp = tmp->i_next) {
		if (!tmp->i_count ||
		    !S_ISCHR (tmp->i_mode) ||
		    MAJOR (tmp->i_rdev) != clone_minor
		)  continue;

		minor_busy[MINOR (tmp->i_rdev)] = 1;
	}
	for (i = 0; i < 256 && minor_busy[i]; i++) ;

	if (i < 256)  new_inode->i_rdev = MKDEV (clone_minor, i);

	restore_flags (flags);

	if (!new_inode->i_rdev) {
		iput (new_inode);
		return -ENOSR;
	}

	/*  How way we should set rights to new inode ???   */
	new_inode->i_mode = inode->i_mode;
	new_inode->i_uid = inode->i_uid;
	new_inode->i_gid = inode->i_gid;

	file->f_inode = new_inode;      /*  new inode   */
	file->f_op = fops;              /*  new file operations   */

	/*  no more needed old (clone) inode   */
	iput (inode);

	if (file->f_op->open)
		return  file->f_op->open (file->f_inode, file);

	return 0;
}

/*  That`s all   */
