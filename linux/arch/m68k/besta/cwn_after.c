/*
 *	Experimental!!! -- Not ended !!!
 * besta/cwn_after.c -- CWN SCSI low level driver for CWN board -
 *			to use after loading local software on the board.
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

#include <linux/stddef.h>
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

#include <asm/setup.h>
#include <asm/pgtable.h>
#include <asm/system.h>
#include <asm/segment.h>
#include <asm/traps.h>

#include "besta.h"
#include "scsi.h"
#include "cwn_soft.h"

#if 0
#undef SC_MAY_DISCONN
#define SC_MAY_DISCONN  0
#endif

#define CWN0_MAJOR     40
#define CWN1_MAJOR     41
#define CWN2_MAJOR     42
#define CWN3_MAJOR     43
#define CWN4_MAJOR     44
#define CWN5_MAJOR     45
#define CWN6_MAJOR     46
#define CWN7_MAJOR     47
#define CWN_FD_MAJOR    48

#define MAX_DEV         5       /* Max number of real  */

static int do_cwn_cmd (int board, int target, char cmd[], int rw, void *addr, int len);
static int cwnicmd (int board, int target, char cmd[], int rw,
				void *addr, int len, int timeout);
static void cwn_scsi_intr (int vec, void *data, struct pt_regs *fp);
static void cwn_board_intr (int vec, void *data, struct pt_regs *fp);

static void do_cwn_request (int board, int target, int major);
static void end_request (int uptodate, int board, int target, int major);

#define SECTOR_MASK (blksize_size[major] &&     \
	blksize_size[major][MINOR(curr->rq_dev)] ? \
	((blksize_size[major][MINOR(curr->rq_dev)] >> 9) - 1) :  \
	((BLOCK_SIZE >> 9)  -  1))

/*  New cwn_board struct, for state after local software has been loaded... */
struct odd {
	char          r0;
	unsigned char reg;
};

struct cwn_board {
	union {
	    unsigned short status[2048];
	    struct {
		struct odd cntrl[4];
		struct odd vect[4];
	    } bim[256];
	} u;
	struct odd intr[1024];
	struct odd reset[1024];
	struct cwn_control_block cwn_cntl;
	short  memory[(128 - 9) * 1024 / 2];
};

#define NUM_CWN_BOARDS  4
struct cwn_board_info {
	volatile struct cwn_board *board_base;
	volatile struct scsi_cmd *scsi_base;
	int     scsi_chain_size;
	volatile char *area_base[8];   /*  XXX: temporary !!!  */
	struct scsi_info_struct *scsi_info;
} cwn_board_info[NUM_CWN_BOARDS];

static int num_cwn = 0;

#define SCSI_CHAIN_SIZE     8
#define SCSI_AREA_SIZE      4096

#define STATUS(x)       (((x) >> 1) & 0x1f)

void cwn_after_init (struct VME_board *VME, int vector[]) {
	volatile struct cwn_board *cwn = (struct cwn_board *) VME->addr;
	int i, j, target, type;
	volatile unsigned char reset;
	volatile struct scsi_init_block *sib;
	volatile struct intr_init_block *iib;
	struct cwn_board_info *priv;
	struct scsi_info_struct *scsi_info;
	char buf[256];      /*  for inquiry, sense, etc.  */

	i = 100000;
	while (!(cwn->cwn_cntl.cntl & SS_OWN_USER) && i--) ;
	if (!(cwn->cwn_cntl.cntl & SS_OWN_USER)) {
	    /*  some local software/hardware errors...  */

	    printk ("FAILED (after), ignore board\n");

	    /*  reset the cwn board for paranoidal reasons...  */
	    reset = cwn->reset[0].reg;

	    VME->present = 0;   /*  Let My People Go! (because resetted)  */
	    return;
	}

	if (num_cwn >= NUM_CWN_BOARDS) {

	    printk ("too many cwn boards (ignore board)\n");

	    /*  reset the cwn board for paranoidal reasons...  */
	    reset = cwn->reset[0].reg;

	    VME->present = 0;   /*  Let My People Go! (because resetted)  */
	    return;
	}

	printk (" done\n");     /*  new cwn software is loaded   */

	scsi_info = kmalloc (sizeof (*scsi_info) * MAX_DEV, GFP_KERNEL);
	if (!scsi_info)  panic ("cwn_init (after): cannot kmalloc\n");
	memset (scsi_info, 0, sizeof (*scsi_info) * MAX_DEV);

	priv = cwn_board_info + num_cwn;

	priv->board_base = cwn;
	priv->scsi_base = (struct scsi_cmd *) cwn->memory;
	priv->scsi_chain_size = SCSI_CHAIN_SIZE;
	priv->scsi_info = scsi_info;

	i = (int) cwn->memory +
		((sizeof (struct scsi_cmd) * SCSI_CHAIN_SIZE + 1023) & ~1023);
	for (target = 0; target < 8; target++) {
		priv->area_base[target] = (char *) i;
		i += SCSI_AREA_SIZE;
	}

	/*  first, set cwn board message stuff...  */
	besta_handlers[vector[GLOBAL_INTR_CHAN]] = cwn_board_intr;
	besta_intr_data[vector[GLOBAL_INTR_CHAN]] = priv;
	cwn->u.bim[0].vect[GLOBAL_INTR_CHAN].reg = vector[GLOBAL_INTR_CHAN];
	cwn->u.bim[0].cntrl[GLOBAL_INTR_CHAN].reg = VME->lev | 0x10;


	sib = (struct scsi_init_block *) cwn->cwn_cntl.cmd_data;
	sib->own_id = 7;
	sib->it = 0;    /*  initiator   */
	sib->scsi_base = offsetof (struct cwn_board, memory);
	sib->scsi_chain_size = SCSI_CHAIN_SIZE;

	cwn->cwn_cntl.cntl = SCSI_INIT;

	while (!(cwn->cwn_cntl.cntl & SS_OWN_USER)) ;

	if (SS_RES (cwn->cwn_cntl.cntl))    /*  XXX: find a better way...  */
		panic ("cwn scsi init phase: res = %d\n",
				    SS_RES (cwn->cwn_cntl.cntl));

	iib = (struct intr_init_block *) cwn->cwn_cntl.cmd_data;
	iib->common_intrs = 1;      /*  do not use a lot of interrupts...  */

	cwn->cwn_cntl.cntl = INTR_INIT;
	while (!(cwn->cwn_cntl.cntl & SS_OWN_USER)) ;


	/*  SCSI initializing stuff starts hear...  */

	printk ("    Probing SCSI devices:\n");

	for (target = 0; target < MAX_DEV; target++) {
	    int res;

	    scsi_info[target].type = TYPE_NONE;
	    scsi_info[target].lun = 0;  /* No lun, no, no ...   */
	    scsi_info[target].state = STATE_FREE;   /*  to avoid bad intrs   */
	    scsi_info[target].major =
		((unsigned char []) {
			CWN0_MAJOR, CWN1_MAJOR, CWN2_MAJOR, CWN3_MAJOR,
			CWN4_MAJOR, CWN5_MAJOR, CWN6_MAJOR, CWN7_MAJOR
		 } )[target];
	    scsi_info[target].name =
		((char * []) { "cwn0", "cwn1", "cwn2", "cwn3",
			       "cwn4", "cwn5", "cwn6", "cwn7" } )[target];
	    scsi_info[target].do_cmd_wait = cwnicmd;
	    scsi_info[target].do_cmd = do_cwn_cmd;
	    scsi_info[target].do_request = do_cwn_request;
	    scsi_info[target].end_request = end_request;

	    scsi_info[target].board = num_cwn;

	    res = cwnicmd (num_cwn, target, scsi_test_unit_ready, 0, 0, 0, 0);

	    if (res && res != TRY_FOR_SENSE)  continue;

	    if (res == TRY_FOR_SENSE) {
		int key;

		res = cwnicmd (num_cwn, target, scsi_request_sense,
							0, buf, 16, 0);
		if (res)  continue;

		key = buf[2] & 0xf;

		if (key == NOT_READY)   /*  spinning up is not done   */
			scsi_info[target].state = STATE_NOT_READY;
		else if (key != UNIT_ATTENTION) /*  may be after reset, etc. */
			continue;
	    }


	    if (cwnicmd (num_cwn, target, scsi_inquiry, 0, buf, 255, 0))
		    continue;

	    printk ("    %d: ", target);
	    for (j = 8; j < buf[4] + 5 && j < 255; j++)
		if (buf[j] >= ' ')  printk ("%c", buf[j]);
		else  printk (" ");
	    printk (", ");

	    type = buf[0] & 0x1f;
	    if (type > FIRST_UNKNOWN_TYPE)  type = FIRST_UNKNOWN_TYPE;

	    printk ("%s ", scsi_inits[type].name);

	    scsi_info[target].type = type;
	    (*scsi_inits[type].init) (target, buf, scsi_info);


	    if (scsi_info[target].type != TYPE_NONE)
		    /*  set  `major -- target & controller' dependence   */
		    scsi_targets[scsi_info[target].major] =
					     target + (num_scsi << 3);
	    else
		scsi_targets[scsi_info[target].major] = -1;

	    printk ("\n");

	} /*  for( ... ) */

	printk ("    done\n");

	/*  set interrupts stuff...   */

	/*  scsi...  */
	besta_handlers[vector[SCSI_INTR_CHAN]] = cwn_scsi_intr;
	besta_intr_data[vector[SCSI_INTR_CHAN]] = priv;
	cwn->u.bim[0].vect[SCSI_INTR_CHAN].reg = vector[SCSI_INTR_CHAN];
	cwn->u.bim[0].cntrl[SCSI_INTR_CHAN].reg = VME->lev | 0x10;

	/*  floppy should be hear...  */

	scsi_infos[num_scsi] = scsi_info;
	num_scsi++;
	num_cwn++;      /*  count this board   */

	return;
}


static int cwnicmd (int board, int target, char scmd[], int rw,
				void *addr, int len, int timeout) {

	struct cwn_board_info *priv = cwn_board_info + board;
	volatile struct cwn_board *cwn = priv->board_base;
	volatile struct scsi_cmd *cmd = priv->scsi_base + target;
	typeof (jiffies) limit;
	int res;

	if (addr && rw == 1)
		memcpy ((void *) priv->area_base[target], addr, len);

	cmd->target = target;
	memcpy ((void *) cmd->cmd, scmd, 12);
	cmd->size = len;
	cmd->addr = (int) priv->area_base[target] - (int) cwn;
	cmd->timeout = (timeout <= 0) ? 3 : ((timeout + HZ-1) / HZ);

	cmd->cntl = SC_DO_CMD | SC_MAY_DISCONN;

	if (timeout <= 0)
	    while (!(cmd->cntl & SS_OWN_USER)) ;
	else {
	    limit = jiffies + timeout;
	    while (!(cmd->cntl & SS_OWN_USER) && jiffies < limit) ;
	    if (jiffies >= limit) {
		cmd->cntl = SC_ABORT_CMD;

		return SOFT_TIMEOUT;
	    }
	}


	res = SS_RES (cmd->cntl);       /*  XXX: ??????  */
	cmd->cntl = 0;

	switch (res) {
	    case 0x00:
		break;  /*  OK   */

	    case RES_BAD_TARGET:
		return CTL_BAD_STATUS;  break;

	    case RES_BAD_PARAM:
	    case RES_NOSYS:
		return CTL_ILL_PARAM;  break;

	    case RES_BAD_CMD:
		return CTL_ILL_CMD;  break;

	    default:
		return CTL_ERROR;
		break;
	}

	switch (STATUS (cmd->status)) {
	    case 0x00:
		break;  /*  OK   */

	    case CHECK_CONDITION:
		return TRY_FOR_SENSE;  break;

	    default:
		return CTL_BAD_STATUS;  break;
	}

	if (addr && rw == 0)
		memcpy (addr, (void *) priv->area_base[target], len);

	return 0;
}


static int do_cwn_cmd (int board, int target, char scmd[],
					int rw, void *addr, int len) {
	struct cwn_board_info *priv = cwn_board_info + board;
	volatile struct cwn_board *cwn = priv->board_base;
	volatile struct scsi_cmd *cmd = priv->scsi_base + target;
	struct scsi_info_struct *scsi_info = priv->scsi_info;
	unsigned short flags;

	save_flags (flags);
	cli();

	while (scsi_info[target].state != STATE_FREE)
	       sleep_on (&scsi_info[target].wait);

	scsi_info[target].state = STATE_CMD;

	if (addr && rw == 1)
		memcpy ((void *) priv->area_base[target], addr, len);

	cmd->target = target;
	memcpy ((void *) cmd->cmd, scmd, 12);
	cmd->size = len;
	cmd->addr = (int) priv->area_base[target] - (int) cwn;

	/*  timeout should be generated by high level code...  */
	cmd->timeout = (scsi_info[target].type == TYPE_DISK) ? 10 : 60;

	cmd->cntl = SC_DO_CMD | SC_INTR_ON_EOP | SC_MAY_DISCONN;

	sleep_on (&scsi_info[target].wait);

	if (scsi_info[target].cmd_ret) {
		restore_flags (flags);
		return scsi_info[target].cmd_ret;
	}

	if (addr && rw == 0)
		memcpy (addr, (void *) priv->area_base[target], len);

	restore_flags (flags);

	return 0;
}


static void do_cwn_request (int board, int target, int major) {
	struct cwn_board_info *priv = cwn_board_info + board;
	volatile struct cwn_board *cwn = priv->board_base;
	volatile struct scsi_cmd *cmd = priv->scsi_base + target;
	struct scsi_info_struct *scsi_info = priv->scsi_info;
	int dev, sect, num_sect, len;
	struct buffer_head *bh;
	struct request *curr;
	struct hd_struct *part;

repeat:
	curr = blk_dev[major].current_request;

	if (!curr || curr->rq_status == RQ_INACTIVE)  return;

	if (MAJOR (curr->rq_dev) != major) {
	    panic ("%s: (major=%d) request list destroyed !\n",
						name(target), major);
	}

	bh = curr->bh;
	if(bh && !buffer_locked(bh)) {
	    panic ("%s: (major=%d) block not locked !\n", name(target), major);
	}

	dev = MINOR (curr->rq_dev);
	part = disk_info(target).part + dev;
	if (dev >= MAX_PARTS ||
	    curr->sector + curr->nr_sectors > 2 * part->nr_sects
	) {
	    end_request (0, board, target, major);
	    goto repeat;
	}

	if (scsi_info[target].state != STATE_FREE)  return;

	sect = curr->sector + 2 * part->start_sect;   /*  512-byte`s blocks  */
	if (scsi_info[target].blksize == 1024) {
		sect >>= 1;
		num_sect = curr->current_nr_sectors >> 1;
	}
	else if (scsi_info[target].blksize == 256) {
		sect <<= 1;
		num_sect = curr->current_nr_sectors << 1;
	} else
	    num_sect = curr->current_nr_sectors;

	len = curr->current_nr_sectors << 9;    /*  in bytes   */
	if (len > SCSI_AREA_SIZE)  panic ("len > SCSI_AREA_SIZE");

	cmd->target = target;
	cmd->size = len;
	cmd->addr = (int) priv->area_base[target] - (int) cwn;

	if (sect > 0x1fffff || num_sect > 0xff) {
	    cmd->cmd[0] = (curr->cmd == WRITE) ? WRITE_10 : READ_10;
	    cmd->cmd[1] = 0;
	    cmd->cmd[2] = (sect >> 24) & 0xff;
	    cmd->cmd[3] = (sect >> 16) & 0xff;
	    cmd->cmd[4] = (sect >> 8) & 0xff;
	    cmd->cmd[5] = sect & 0xff;
	    cmd->cmd[6] = 0;
	    cmd->cmd[7] = (num_sect >> 8) & 0xff;
	    cmd->cmd[8] = num_sect & 0xff;
	    cmd->cmd[9] = 0;

	} else {
	    cmd->cmd[0] = (curr->cmd == WRITE) ? WRITE_6 : READ_6;
	    cmd->cmd[1] = (sect >> 16) & 0x1f;
	    cmd->cmd[2] = (sect >> 8) & 0xff;
	    cmd->cmd[3] = sect & 0xff;
	    cmd->cmd[4] = num_sect;
	    cmd->cmd[5] = 0;
	}

	if (curr->cmd == WRITE)
		memcpy ((void *) priv->area_base[target], curr->buffer, len);

	scsi_info[target].state = STATE_IO;

	cmd->timeout = 15;  /*  Linux way...  */
#if 1
	cmd->cntl = SC_DO_CMD | SC_INTR_ON_EOP | SC_MAY_DISCONN | SC_BLOCK_XFER;
#else
	cmd->cntl = SC_DO_CMD | SC_INTR_ON_EOP | SC_MAY_DISCONN;
#endif

	return;
}


static void end_request (int uptodate, int board, int target, int major) {
	struct cwn_board_info *priv = cwn_board_info + board;
	struct request *curr = blk_dev[major].current_request;
	struct buffer_head *bh;

	if (!uptodate) {
	    printk("end_request: %s error, dev %04lX, sector %lu\n",
			(curr->cmd == READ) ? "read" : "write",
			(unsigned long) curr->rq_dev, curr->sector >> 1);

	    curr->nr_sectors--;
	    curr->nr_sectors &= ~SECTOR_MASK;
	    curr->sector += (BLOCK_SIZE/512);
	    curr->sector &= ~SECTOR_MASK;
	}
	else if (curr->cmd == READ)
	    /*  copy the needable data...  */
	    memcpy (curr->buffer, (void *) priv->area_base[target],
					    curr->current_nr_sectors << 9);

	if (!curr->bh && curr->nr_sectors > 0)  return;   /*  yet not ready   */

	curr->errors = 0;

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


static void cwn_scsi_intr (int vec, void *data, struct pt_regs *fp) {
	struct cwn_board_info *priv = data;
	volatile struct cwn_board *cwn = priv->board_base;
	struct scsi_info_struct *scsi_info = priv->scsi_info;
	volatile struct scsi_cmd *cmd = priv->scsi_base;
	int target, err;

	for (target = 0; target < MAX_DEV; target++, cmd++) {

	    if (!(cmd->cntl & SS_OWN_USER))  continue;

	    if (scsi_info[target].state == STATE_FREE) {
		cmd->cntl = 0;
		printk("%s: interrupt while inactive\n", name(target));
		continue;
	    }

	    err = 0;

	    if (scsi_info[target].req_sense) {

		if (SS_RES (cmd->cntl) ||
		    STATUS (cmd->status) != GOOD
		) {
		    scsi_info[target].req_sense = 0;
		    printk ("%s: Bad regsense (status = 0x%02x)\n",
						name(target), cmd->status);
		    err = CTL_ERROR;
		} else
		    memcpy (&scsi_info[target].sense_buf,
				(void *) priv->area_base[target],
				    sizeof (scsi_info[target].sense_buf));

	    } else {

		switch (SS_RES (cmd->cntl)) {
		    case 0x00:   /*  OK   */
			break;

		    case RES_BAD_PARAM:
		    case RES_NOSYS:
			printk ("%s: Illegal Parameter\n", name(target));
			err = CTL_ILL_PARAM;
			break;

		    case RES_BAD_CMD:
			printk ("%s: Illegal Command\n", name(target));
			err = CTL_ILL_CMD;
			break;

		    case RES_BAD_TARGET:
			printk ("%s: Target say fuck\n", name(target));
			err = CTL_BAD_STATUS;
			break;

		    case RES_ABORTED:
		    case RES_RESET:
			err = CTL_ERROR;

		    default:
			printk ("%s: Unknown error %d\n", name(target),
							SS_RES (cmd->cntl));
			err = CTL_ERROR;
			break;
		}

		if (!err)
		    switch (STATUS (cmd->status)) {
			case GOOD:
			    break;  /*  OK   */

			case CHECK_CONDITION:
			    cmd->target = target;
			    cmd->size = sizeof (scsi_info[target].sense_buf);
			    cmd->addr = (int) priv->area_base[target] -
								(int) cwn;
			    memcpy ((void *) cmd->cmd, scsi_request_sense, 6);

			    cmd->cntl = 1;  /*  linux way -- 0.5   */
			    cmd->cntl = SC_DO_CMD | SC_INTR_ON_EOP |
							    SC_MAY_DISCONN;

			    scsi_info[target].req_sense = 1;
			    continue;
			    break;

			case CONDITION_GOOD:
			case BUSY:
			case INTERMEDIATE_GOOD:
			case INTERMEDIATE_C_GOOD:
			case RESERVATION_CONFLICT:
			case COMMAND_TERMINATED:
			case QUEUE_FULL:

			default:
			    err = CTL_BAD_STATUS;
			    printk ("%s: Bad status 0x%02x\n",
					    name(target), cmd->status);
			    break;
		    }

	    }

	    if (!scsi_info[target].inthandler) {
		    cmd->cntl = 0;
		    printk("%s: interrupt from unused target\n", name(target));
		    scsi_info[target].state = STATE_FREE;   /* avoid other  */
		    continue;
	    }

	    cmd->cntl = 0;
	    scsi_info[target].inthandler (target, err, scsi_info);

	} /* for( ... )   */

	return;
}


static void cwn_board_intr (int vec, void *data, struct pt_regs *fp) {
	struct cwn_board_info *priv = data;
	volatile struct cwn_board *cwn = priv->board_base;

	if (cwn->cwn_cntl.msg_valid) {
	    printk ("cwn at 0x%08x says: %s (%d %d %02x %02x)\n",
			(int) cwn,
			cwn->cwn_cntl.msg_string,
			cwn->cwn_cntl.msg_state,
			cwn->cwn_cntl.msg_event,
			cwn->cwn_cntl.msg_aux_stat,
			cwn->cwn_cntl.msg_stat);
	}

	cwn->cwn_cntl.msg_valid = 0;

	return;
}
