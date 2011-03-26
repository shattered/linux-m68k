/*
 * besta/scsi.c -- Common SCSI driver for Bestas (Both HCPU30 and CWN and...).
 *		   Main SCSI routines.
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


/*  major -- target & controller  dependence   */
char scsi_targets[256] = { [ 0 ... 255 ] = -1 };

#define MAX_SCSI_CTL    8
struct scsi_info_struct *scsi_infos[MAX_SCSI_CTL] = { NULL, };
int num_scsi = 0;

extern void scsi_disk_init (int target, char *inquiry_buffer,
					struct scsi_info_struct *scsi_info);
extern void scsi_tape_init (int target, char *inquiry_buffer,
					struct scsi_info_struct *scsi_info);

static void scsi_unsup_init (int target, char *inquiry_buffer,
					struct scsi_info_struct *scsi_info) {

	printk ("(unsupported)");
	scsi_info[target].type = TYPE_NONE;

	return;
}

struct scsi_init_struct scsi_inits[] = {
/* 0 */ { "disk",           scsi_disk_init },
/* 1 */ { "tape",           scsi_tape_init },
/* 2 */ { "printer",        scsi_unsup_init },
/* 3 */ { "processor",      scsi_unsup_init },
/* 4 */ { "WORM",           scsi_unsup_init },
/* 5 */ { "CD-ROM",         scsi_unsup_init },
/* 6 */ { "scanner",        scsi_unsup_init },
/* 7 */ { "optical device", scsi_unsup_init },
/* 8 */ { "medium changer", scsi_unsup_init },
/* 9 */ { "communications", scsi_unsup_init },
/* ? */ { "unknown type",   scsi_unsup_init }
};


/*  for SCSI command emulation    */
#define SIOCSETEMU      (('s'<<8)|19)   /* ioctl: set cmd emulation     */
#define SIOCUNSETEMU    (('s'<<8)|20)   /* ioctl: unset cmd emulation   */
#define SIOCCLEAREMU    (('s'<<8)|21)   /* ioctl: unset all cmd emulation */

struct cmd_emu {
	int           in_use;
	unsigned char cmd[16];
	unsigned char mask[16];
	char          comm[16];
	unsigned char data[256];
} cmd_emu[8];
#define MAX_CMD_EMU     (sizeof (cmd_emu) / sizeof (struct cmd_emu))

int scsi_ioctl (int target, struct scsi_info_struct *scsi_info,
		    struct file *file, unsigned int cmd, unsigned long arg) {
	int err;
	int i;

	switch (cmd) {

	    case SIOCTRSPMOD:
		{
		    struct trsp_mode {
			unsigned int    csize;      /* command size  */
			unsigned char  *tcommand;   /* command address  */
			unsigned int    wsize;      /* write data size  */
			void           *wdata;      /* write data address */
			unsigned int    rsize;      /* read data size  */
			void           *rdata;      /* read data address  */
			unsigned int    tlen;       /* special length size */
		    } trsp;
		    unsigned char tcommand[15];
		    char buf[512];

		    err = verify_area (VERIFY_READ, (struct trsp_mode *) arg,
					       sizeof (struct trsp_mode));
		    if (err)  return err;

		    memcpy_fromfs (&trsp, (struct trsp_mode *) arg,
					       sizeof (struct trsp_mode));

		    err = verify_area (VERIFY_READ, trsp.tcommand, 15);
		    if (err)  return err;
		    memcpy_fromfs (tcommand, trsp.tcommand, 15);

		    i = tcommand[0];
		    if (i > 14)  return -EINVAL;
		    for ( ; i < 14; i++)  tcommand[1+i] = 0;

		    if (trsp.wsize > 1) {

			if (trsp.wsize > sizeof (buf))  return -EINVAL;
			err = verify_area (VERIFY_READ, trsp.wdata,
								trsp.wsize);
			if (err)  return err;

			memcpy_fromfs (buf, trsp.wdata, trsp.wsize);
			err = do_scsi_cmd (target, &tcommand[1], 1,
							buf, trsp.wsize);
			return err;

		    } else {

			if (trsp.rsize > sizeof (buf))  return -EINVAL;
			err = verify_area (VERIFY_WRITE, trsp.rdata,
								trsp.rsize);
			if (err)  return err;

			/*  May be it should be emulated.  */
			for (i = 0; i < MAX_CMD_EMU; i++) {
			    if (cmd_emu[i].in_use &&
				!strcmp (current->comm, cmd_emu[i].comm)
			    ) {
				int j;

				for (j = 0;
				     j < tcommand[0] &&
				     (cmd_emu[i].mask[j] != 0 ||
				      cmd_emu[i].cmd[j] == tcommand[1+j]);
				     j++
				);
				if (j == tcommand[0])  break;
			    }
			}

			if (i == MAX_CMD_EMU) {  /*  transparent way   */

			    err = do_scsi_cmd (target, &tcommand[1], 0,
							    buf, trsp.rsize);
			    if (err)  return err;

			} else
			    memcpy (buf, cmd_emu[i].data,
						sizeof (cmd_emu[i].data));

			memcpy_tofs (trsp.rdata, buf, trsp.rsize);

			return 0;
		    }

		    break;
		}  /* SIOCTRSPMOD  */

	    case SIOCSETEMU:
		err = verify_area (VERIFY_READ, (void *) arg,
						sizeof (struct cmd_emu));
		if (err)  return err;

		for (i = 0; i < MAX_CMD_EMU; i++)
		    if (!cmd_emu[i].in_use) break;
		if (i == MAX_CMD_EMU)  return -ENOMEM;

		memcpy_fromfs (&cmd_emu[i], (void *) arg,
						sizeof (struct cmd_emu));
		cmd_emu[i].in_use = 1;

		return 0;
		break;

	    case SIOCUNSETEMU:
		{   struct cmd_emu tmp;

		    err = verify_area (VERIFY_READ, (void *) arg,
					(int) &((struct cmd_emu *) 0)->data);
		    if (err)  return err;

		    memcpy_fromfs (&tmp, (void *) arg,
					(int) &((struct cmd_emu *) 0)->data);

		    for (i = 0; i < MAX_CMD_EMU; i++) {
			if (cmd_emu[i].in_use &&
			    !strcmp (cmd_emu[i].comm, tmp.comm)
			) {
			    int j;

			    for (j = 0;
				 j < sizeof (tmp.cmd) &&
				 (cmd_emu[i].mask[j] != 0 ||
				  cmd_emu[i].cmd[j] == tmp.cmd[j]);
				 j++
			    );
			    if (j == sizeof (tmp.cmd))   /*  found   */
				    cmd_emu[i].in_use = 0;
			}
		    }

		    return 0;
		    break;
		}  /*  SIOCUNSETEMU   */

	    case SIOCCLEAREMU:
		for (i = 0; i < MAX_CMD_EMU; i++)
			cmd_emu[i].in_use = 0;
		return 0;
		break;


	    /*  Ordinary Linux stuff.   */
#ifndef SCSI_IOCTL_DOORLOCK
/*  usual place is in scsi/scsi_ioctl.h , but so many extras...  */
#define SCSI_IOCTL_SEND_COMMAND         1
#define SCSI_IOCTL_TEST_UNIT_READY      2
#define SCSI_IOCTL_DOORLOCK        0x5380   /* lock the eject mechanism */
#define SCSI_IOCTL_DOORUNLOCK      0x5381   /* unlock the mechanism   */
#endif  /*  SCSI_IOCTL_SEND_COMMAND   */

	    case SCSI_IOCTL_GET_IDLUN:
		err = verify_area(VERIFY_WRITE, (void *) arg, 2*sizeof(long));
		if (err)  return err;

		/*  don`t know what can we send also...  */
		put_user (target, (unsigned long *) arg);
		put_user (0, (unsigned long *) arg + 1);

		return 0;
		break;

	    case SCSI_IOCTL_PROBE_HOST:
		/*  Brrrrr.....   He-e-e-e-e...   */
		{   unsigned long len;
		    static char name[] = "scsi";

		    err = verify_area (VERIFY_READ, (void *) arg,
							sizeof (long));
		    if (err)  return err;

		    len = get_user ((unsigned long *) arg);
		    len = len < sizeof (name) ? len : sizeof (name);

		    err = verify_area (VERIFY_WRITE, (void *) arg, len);
		    if (err)  return err;

		    memcpy_tofs ((void *) arg, name, len);

		    return 1;   /* as it want this   */
		}
		break;

	    /*  This interface is depreciated - users should use
	       the scsi generics interface instead, as this is
	       a more flexible approach to performing generic SCSI commands
	       on a device.
	    */

	    case SCSI_IOCTL_SEND_COMMAND:
		{   int inlen, outlen, cmdlen;
		    char *cmd_in;
		    char cmd[12] = { 0,0,0,0,0,0,0,0,0,0,0,0 };
		    char *buf;

		    if (!suser())  return -EACCES;

		    err = verify_area (VERIFY_READ, (void *) arg,
							2 * sizeof (long) + 1);
		    if (err)  return err;

		    inlen = get_user ((unsigned int *) arg);
		    outlen = get_user (((unsigned int *) arg) + 1);

		    if (inlen && outlen)  return -EINVAL;

		    /*  If the users needs to transfer more data than this,
		       they  should use scsi_generics instead.
		    */
#define MAX_BUF 4096
		    if (inlen > MAX_BUF)  inlen = MAX_BUF;
		    if (outlen > MAX_BUF)  outlen = MAX_BUF;
#undef MAX_BUF

		    cmd_in = (char *) (((int *) arg) + 2);

		    cmdlen = ((char []) { 6, 10, 10, 12, 12, 12, 10, 10 })
					[((get_user (cmd_in)) >> 5) & 0x7];

		    err = verify_area (VERIFY_READ, cmd_in, cmdlen + inlen);
		    if (err)  return err;

		    memcpy_fromfs (cmd, cmd_in, cmdlen);

		    if (outlen) {
			err = verify_area (VERIFY_WRITE, cmd_in, outlen);
			if (err)  return err;

			buf = kmalloc (outlen, GFP_KERNEL);
			if (!buf)  return -ENOMEM;

			err = do_scsi_cmd (target, cmd, 0, buf, outlen);
			if (!err)  memcpy_tofs (cmd_in, buf, outlen);

			kfree (buf);
		    }
		    else {
			if (inlen) {
			    buf = kmalloc (inlen, GFP_KERNEL);
			    if (!buf)  return -ENOMEM;

			    memcpy_fromfs (buf, cmd_in + cmdlen, inlen);
			} else
			    buf = NULL;

			err = do_scsi_cmd (target, cmd, 1, buf, inlen);

			if (buf)  kfree (buf);
		    }

		    if (err) {
			int res = verify_area (VERIFY_WRITE, cmd_in,
					sizeof (scsi_info[target].sense_buf));
			if (res)  return res;

			memcpy_tofs (cmd_in, scsi_info[target].sense_buf,
					sizeof (scsi_info[target].sense_buf));
		    }

		    return err;
		}
		break;

	    case SCSI_IOCTL_DOORLOCK:

		/*  but for non-removable devices ???   */
		return do_scsi_cmd (target, scsi_disall_md_removal, 1, 0, 0);
		break;

	    case SCSI_IOCTL_DOORUNLOCK:

		/*  but for non-removable devices ???   */
		return do_scsi_cmd (target, scsi_all_md_removal, 1, 0, 0);
		break;


	    case SCSI_IOCTL_TEST_UNIT_READY:

		return do_scsi_cmd (target, scsi_test_unit_ready, 0, 0, 0);
		break;

	    default:
		return -EINVAL;
		break;
	}

	return 0;
}

