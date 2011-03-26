/*
 * besta/cwn.c -- SCSI-low level driver and floppy driver for CWN board.
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

#include <asm/setup.h>
#include <asm/pgtable.h>
#include <asm/system.h>
#include <asm/segment.h>
#include <asm/traps.h>

#include "besta.h"
#include "scsi.h"


#define CWN0_MAJOR     40
#define CWN1_MAJOR     41
#define CWN2_MAJOR     42
#define CWN3_MAJOR     43
#define CWN4_MAJOR     44
#define CWN5_MAJOR     45
#define CWN6_MAJOR     46
#define CWN7_MAJOR     47
#define CWN_FD_MAJOR    48

#define CWN_CTL_NUM     0       /*  while ...   */

struct odd {
	char          r0;
	unsigned char reg;
};

struct cmd {
	unsigned short cmd;
	unsigned short par0;
	unsigned long  par1;
	unsigned long  par2;
	unsigned short par3;
	unsigned short par4;
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
	unsigned short version;
	unsigned short filler0[127];
	struct cmd cmd0;
	struct cmd cmd_chain0[31];
	unsigned short iobuf0[1024];
	unsigned short iobuf1[1024];
	struct cmd cmd_chain1[32];
	struct cmd cmd1;
	unsigned short filler1[1400];
	unsigned short memory[56*1024];
};

struct ctlstat {
	unsigned char target_id;
	unsigned char target_sect_size;
	unsigned char string_delimiter;
	unsigned char scsi_bus_state;
	unsigned char target_mode;
	unsigned char auto_dubl;
	unsigned char backup;       /*  ???   */
	unsigned char write_buff;
	unsigned char target_locks;
	unsigned char filler;
	unsigned long addr_offset;
	unsigned long sect_offset;
} __attribute__ ((packed));


#define MAX_DEV         5       /* Max number of real  */

struct scsi_info_struct cwn_info[MAX_DEV] = { {0, }, };

static struct cwn_chain {
	struct cmd cmd;
	unsigned char scsi_cmd[16];
} cwn_chain[MAX_DEV] = { { {0, }, {0, } }, };

static void run_cwn_chain (void);

static struct cwn_area {
	unsigned start_read;
	unsigned start_write;
	unsigned short read_dirty;
	unsigned short write_dirty;
	unsigned read_sect;
	unsigned read_area;
	unsigned write_sect;
	unsigned short write_head;
	unsigned short write_count;
	unsigned write_area;
} cwn_area[MAX_DEV] = { {0, }, };


static void *cwn_addr = NULL;       /*  should be array later...  */
static int cwn_active_target = -1;

static int do_cwn_cmd (int board, int target, char cmd[], int rw,
						    void *addr, int len);
static int cwnicmd (int board, int target, char cmd[], int rw,
				void *addr, int len, int timeout);
static void cwn_intr (int vec, void *data, struct pt_regs *fp);
static void cwn_fd_intr (int vec, void *data, struct pt_regs *fp);

static void do_cwn_request (int board, int target, int major);
static void end_request (int uptodate, int board, int target, int major);

#define SECTOR_MASK (blksize_size[major] &&     \
	blksize_size[major][MINOR(curr->rq_dev)] ? \
	((blksize_size[major][MINOR(curr->rq_dev)] >> 9) - 1) :  \
	((BLOCK_SIZE >> 9)  -  1))


/*      THE  FLOPPY  STUFF        */

#define FD_STATE_FREE      0
#define FD_STATE_IO        1
#define FD_STATE_CMD       2
#define FD_STATE_FORMAT    3
#define FD_STATE_BREAK     6

static int cwn_fd_open (struct inode *inode, struct file *filp);
static void cwn_fd_release (struct inode *inode, struct file *file);
static void do_fd_request (void);
static void fd_end_request (int uptodate, int major);
static int cwn_fd_ioctl( struct inode *inode, struct file *file, unsigned int
		       cmd, unsigned long arg );
static int do_fd_cmd (int dev, int cmd);


static struct file_operations cwn_fd_fops = {
	NULL,                   /* lseek - default */
	block_read,             /* read - general block-dev read */
	block_write,    /* write - general block-dev write */
	NULL,                   /* readdir - bad */
	NULL,                   /* select */
	cwn_fd_ioctl,             /* ioctl */
	NULL,                   /* mmap */
	cwn_fd_open,              /* open */
	cwn_fd_release,   /* release */
	block_fsync,            /* fsync */
	NULL,                   /* fasync */
	NULL,                   /* media_change */
	NULL,                   /* revalidate */
};

struct cwn_fd_par {
	unsigned char heads;
	unsigned char cylinders;
	unsigned char sects_per_cyl;
	unsigned char first_sect;
	unsigned char bs_bits;
	unsigned char density;
	unsigned char pattern;
	unsigned char table_num;
	unsigned char min_sect;
	unsigned char steprate;
	unsigned char table[20];

} cwn_fd_par[] = {
/*auto*/{ 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, { 0, }},
/*800*/ { 0x2,  80, 0xa, 0x0, 0x3, 0x1,0xe5, 0x8, 0x0, 0x3,
			{0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19} },
/*1200*/{ 0x0,  /*  not supported   */                           },
/*720*/ { 0x2,  80,0x12, 0x0, 0x2, 0x1,0xe5, 0x0, 0x1, 0x3, { 0, }},
/*360*/ { 0x2,  40,0x12, 0x0, 0x2, 0x1,0xe5, 0x0, 0x1, 0x3, { 0, }},
/*1440*/{ 0x0,  /*  not supported   */                           },
/*640*/ { 0x2,  80,0x20, 0x0, 0x1, 0x1,0xe5, 0x0, 0x1, 0x3, { 0, }}
};

#define FD_MINORS (sizeof (cwn_fd_par) / sizeof (struct cwn_fd_par))

static struct hd_struct cwn_fd_part[FD_MINORS] = {{0,0}, };
static int      cwn_fd_blocksizes[FD_MINORS] = { 0, };

/*  Must be first not autoprobe minor.  */
#define FIRST_REAL_DEV  1
static int fd_real_dev = FIRST_REAL_DEV;

static int cwn_fd_busy = 0;
static int fd_cmd_ret = 0;
static int fd_in_probe = 0;
static int fd_access_count = 0;
static int fd_media_tested = 0;

static struct wait_queue *fd_cmd_wait = NULL;



/*  Returns 1 if we can assume the board has been resetted and clean.   */
static int cwn_after_reset (volatile struct cwn_board *cwn) {
	int i;

	/*  dog timer should be enabled   */
	if (!(cwn->u.status[0] & 0x0400))  return 0;

	/*  BIM should be in initial state   */
	for (i = 0; i < 4; i++) {
	    if (cwn->u.bim[0].cntrl[i].reg != 0)  return 0;
	    if (cwn->u.bim[0].vect[i].reg != VEC_UNINT)  return 0;
	}

	/*  command ports should be in initial clean state   */
	if (!(cwn->cmd0.cmd & 0x8000) ||
	    !(cwn->cmd1.cmd & 0x8000)
	)  return 0;

	/*  About tests above:
	    Be careful: theoretically there are no enough tests...
	*/
	return 1;
}


void cwn_init (struct VME_board *VME, int on_off) {
	volatile struct cwn_board *cwn = (struct cwn_board *) VME->addr;
	volatile unsigned char reset;
	struct scsi_info_struct *scsi_info = cwn_info;
	int i, j, type, target;
	int vector[4];
	struct ctlstat *ctlstat;
	unsigned char *inquiry_buffer;

	if (on_off) {       /*  deinit   */

	    /*  reset the board into initial state. Most correct way...  */
	    VME_probe (&cwn->reset[0].reg, 0, PROBE_READ, PORT_BYTE);

	    /*  set bim into initial state too   */
	    for (i = 0; i < 4; i++) {
		cwn->u.bim[0].cntrl[i].reg = 0;
		cwn->u.bim[0].vect[i].reg = VEC_UNINT;
	    }

	    return;
	}


	if (VME_probe (&cwn->version, 0, PROBE_READ, PORT_BYTE)) {
	    printk ("    no %s at 0x%08x\n",
		    (VME->name ? VME->name : "board"), VME->addr);
	    return;
	}

	cwn_addr = (void *) VME->addr;

	/*  get four vectors for bim...  */
	if (VME->vect < 0)
	    for (i = 0; i < 4; i++)  vector[i] = get_unused_vector();

	else {
	    if (VME->vect - VME->lev == VEC_SPUR) {
		printk ("Cannot register cwn board at 0x%08x: "
			"don`t support autovectors\n", VME->addr);
		return;
	    }

	    for (i = 0; i < 4; i++)  vector[i] = VME->vect + i;
	    for (i = 0; i < 4; i++) {
		if (besta_handlers[vector[i]] != NULL) {
		    printk ("Cannot register cwn board at 0x%08x: "
			    "some of vectors 0x%x 0x%x 0x%x 0x%x "
			    "already in use\n", VME->addr,
			    vector[0], vector[1], vector[2], vector[3]);
		    return;
		}
	    }
	}


	printk ("  0x%08x: ", VME->addr);

	/*  If this board is in HCPU30 configuration or
	  this is an additional board (second, third, ...) in
	  CP31 configuration  we should check and reset the board
	  into initial state.
	    Otherwise this board is already used and initialized by boot,
	  so, it is not needable to reset it.
	*/

	if ((boot_info.machtype == MACH_BESTA_HCPU30 ||
	     VME->addr != 0xfca00000) &&
	    !cwn_after_reset (cwn)
	) {
	    /*  There is not "after reset immediately" state,
	      and this board is not used by boot loader.
	      So, it is needable to reset the board for autotest, etc.
		This stuff is needable because cwn autotest continues
	      very long (about 4 seconds) at the boot time.
	    */

	    printk ("testing.");

	    /*  reset the cwn board    */

	    reset = cwn->reset[0].reg;

	    for (i = 0; i < 1000000; i++) ;     /*  paranoia   */
	    printk (".");

	    for (i = 0; i < 8; i++) {   /*  no longer than 8 seconds...  */
		int start = jiffies;

		while ((cwn->u.status[0] & 0x0400) == 0 &&
		       jiffies < start + HZ
		) ;
		if (cwn->u.status[0] & 0x0400)  break;

		printk (".");
	    }

	    if ((cwn->u.status[0] & 0x0400) == 0) {
		printk ("FAILED! (ignore board)\n");

		return;
	    }

	    printk ("OK: ");

#if 0
	} else {
	    cwn->cmd1.cmd = 0x0061;     /*  SCSIRST   */
	    while (!(cwn->cmd1.cmd & 0x8000)) ;
#endif
	}

	VME->present = 1;       /*  OK, board is present   */

	printk ("CWN v=%x, ser=%x, ", cwn->version >> 8,
						cwn->version & 0xff);

	/*  maininit cmd for board   */
	/*  Should it be done for both channels ???  */

	memcpy ((void *) cwn->iobuf1, ((char []) { 0x7, 0x04, 0 }), 3);
	cwn->cmd1.par1 = (int) &((struct cwn_board *) 0)->iobuf1;
	cwn->cmd1.cmd = 0x000d;     /*  MAININIT   */
	while (!(cwn->cmd1.cmd & 0x8000)) ;


	/*  get the board ctl status   */
	cwn->cmd1.cmd = 0x0044;     /*  CTLSTAT   */
	while (!(cwn->cmd1.cmd & 0x8000)) ;

	ctlstat = (struct ctlstat *) (((int) cwn) + cwn->cmd1.par1);

	printk ("scsi: id=%x, ", ctlstat->target_id);

	switch (ctlstat->scsi_bus_state) {
	    case 0:
		printk ("not connected");  break;
	    case 1:
		printk ("initiator");  break;
	    case 2:
		printk ("object");  break;
	    default:
		break;
	}

	printk (", sect=%db", ctlstat->target_sect_size << 8);

	if (ctlstat->target_mode) {     /*  but we want to be initiator...  */
	    cwn->cmd1.cmd = 0x0062;     /*  TARGMOD  */
	    while (!(cwn->cmd1.cmd & 0x8000)) ;
	}

	if (ctlstat->auto_dubl) {       /*  don`t want this...  */
	    cwn->cmd1.cmd = 0x000c;     /*  WRPARAL   */
	    while (!(cwn->cmd1.cmd & 0x8000)) ;
	}

	if (ctlstat->write_buff) {      /*  don`t want this...  */
	    cwn->cmd1.cmd = 0x000b;     /*  BUFFWR   */
	    while (!(cwn->cmd1.cmd & 0x8000)) ;
	}

	if (ctlstat->addr_offset) {     /*  addr offset, should be zero...  */
	    cwn->cmd1.par1 = 0x00000000;
	    cwn->cmd1.cmd = 0x0003;     /*  SETOFFS   */
	    while (!(cwn->cmd1.cmd & 0x8000)) ;
	}

	if (ctlstat->sect_offset) {     /*  blocks offset, should be zero... */
	    cwn->cmd1.par1 = 0x00000000;
	    cwn->cmd1.cmd = 0x0008;     /*  DEFOFFS   */
	    while (!(cwn->cmd1.cmd & 0x8000)) ;
	}


	/*  do not want any hashing...  */
	for (target = 0; target < 7; target++) {

	    cwn->cmd1.par1 = (int) &((struct cwn_board *) 0)->iobuf1;
	    memset ((void *) cwn->iobuf1, 0, 12);
	    cwn->cmd1.par3 = 0;

	    cwn->cmd1.cmd = (target << 9) | 0x000a;     /*  SUNPARM   */
	    while (!(cwn->cmd1.cmd & 0x8000)) ;
	}

	/*  paranoia ???  */
	cwn->cmd1.cmd = 0x005f;     /*  CLRHASH   */
	while (!(cwn->cmd1.cmd & 0x8000)) ;

	printk ("\n");


	/*  SCSI initializing stuff starts hear...  */

	inquiry_buffer = (unsigned char *) cwn->iobuf1;

	printk ("    Probing SCSI devices:\n");

	for (target = 0; target < MAX_DEV; target++) {
	    int res;

	    /*  May be it is better to use static initialization for this one,
	       but  struct fields order may be changed later etc...
	    */
	    scsi_info[target].type = TYPE_NONE;
	    scsi_info[target].lun = 0;  /* No lun, no, no ...   */
	    scsi_info[target].state = STATE_FREE;   /*  To avoid bad ints  */
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


	    res = cwnicmd (0, target, scsi_test_unit_ready, 0, 0, 0, 0);

	    if (res == TRY_FOR_SENSE) {
		int key;

		res = cwnicmd (0, target, scsi_request_sense, 0,
					    (void *) cwn->iobuf1, 16, 0);
		if (res)  continue;

		key = ((char *) &cwn->iobuf1)[2] & 0xf;

		if (key != NOT_READY && key != UNIT_ATTENTION)  continue;

		scsi_info[target].state = STATE_NOT_READY;
	    }

	    if (cwnicmd (0, target, scsi_inquiry, 0, inquiry_buffer, 255, 0))
		    continue;

	    printk ("    %d: ", target);
	    for (j = 8; j < inquiry_buffer[4] + 5 && j < 255; j++)
		if (inquiry_buffer[j] >= ' ')
		    printk ("%c", inquiry_buffer[j]);
		else
		    printk (" ");
	    printk (", ");

	    type = inquiry_buffer[0] & 0x1f;
	    if (type > FIRST_UNKNOWN_TYPE)  type = FIRST_UNKNOWN_TYPE;

	    printk ("%s ", scsi_inits[type].name);

	    scsi_info[target].type = type;
	    (*scsi_inits[type].init) (target, inquiry_buffer, scsi_info);


	    if (scsi_info[target].type != TYPE_NONE)
		    /*  set  `major -- target & controller' dependence   */
		    scsi_targets[scsi_info[target].major] =
					     target + (CWN_CTL_NUM << 3);
	    else
		scsi_targets[scsi_info[target].major] = -1;

	    printk ("\n");

	} /*  for( ... ) */

	printk ("    done\n");

	scsi_infos[CWN_CTL_NUM] = scsi_info;


	/*  cwn 112k memory management   */

	j = 0;
	for (target = 0; target < MAX_DEV; target++)
	    if (scsi_info[target].type != TYPE_NONE)  j++;

	if (j) {
	    int area_size =  (sizeof (cwn->memory) / j) & ~1023;
					/*  be careful for large block sizes  */
	    int area_start = (int) &((struct cwn_board *) 0)->memory;

	    for (target = 0; target < MAX_DEV; target++) {
		if (scsi_info[target].type == TYPE_NONE)  continue;

		if (scsi_info[target].type == TYPE_DISK) {
#if 0
		    int needed_read = disk_info(target).sects_per_track *
						    scsi_info[target].blksize;
#else
		    int needed_read = 4 * 1024;
#endif
		    int needed_write = 16 * 1024;   /*  ???  */

		    if (needed_read == 0)  needed_read = 16 * 1024;   /* ??? */

		    if (needed_read + needed_write > area_size) {
			int common = needed_read + needed_write;

			needed_read = (needed_read * area_size) / common;
			needed_write = (needed_write * area_size) / common;
		    }

		    needed_read &= ~1023;
		    needed_write &= ~1023;

		    cwn_area[target].start_read = area_start;
		    cwn_area[target].start_write = area_start + needed_read;
		    cwn_area[target].read_dirty = 0;
		    cwn_area[target].write_dirty = 0;
		    cwn_area[target].read_area =
				    needed_read / scsi_info[target].blksize;
		    cwn_area[target].write_area =
				    needed_write / scsi_info[target].blksize;
		    cwn_area[target].write_count = 0;   /*  paranoia   */
		    cwn_area[target].write_head = 0;    /*  paranoia   */

		}
		else if (scsi_info[target].type == TYPE_TAPE) {

		    cwn_area[target].start_read = area_start;
		    cwn_area[target].start_write = area_start;
		    cwn_area[target].read_dirty = 0;
		    cwn_area[target].write_dirty = 0;
		    cwn_area[target].read_area = area_size;
		    cwn_area[target].write_area = area_size;
		    cwn_area[target].write_count = 0;
		}
		else ;  /*  should not be reached   */

		area_start += area_size;

	    }  /*  for ( ... )   */
	}


	/*   Floppy  stuff   */

	for (i = 0; i < FD_MINORS; i++) {
	    cwn_fd_part[i].start_sect = 0;
	    cwn_fd_part[i].nr_sects =
		    (cwn_fd_par[i].cylinders *
			cwn_fd_par[i].sects_per_cyl) >>
			    (3 - cwn_fd_par[i].bs_bits);
	    cwn_fd_blocksizes[i] = 128 << cwn_fd_par[i].bs_bits;

	    /*  buffer.c don`t want secsize less than 512 bytes...  */
	    if (cwn_fd_blocksizes[i] < 512)
		    cwn_fd_blocksizes[i] = 512;
	}

	if (register_blkdev (CWN_FD_MAJOR, "cwn_fd", &cwn_fd_fops)) {
	    printk ("Unable to get major %d \n", CWN_FD_MAJOR);
	    return;
	}

	blk_dev[CWN_FD_MAJOR].request_fn = do_fd_request;
	read_ahead[CWN_FD_MAJOR] = 0;
	blksize_size[CWN_FD_MAJOR] = cwn_fd_blocksizes;


	/*  set interrupts stuff...   */

	for (i = 0; i < 4; i++) {
	    cwn->u.bim[0].vect[i].reg = vector[i];
	    cwn->u.bim[0].cntrl[i].reg = VME->lev | 0x10;

	    besta_intr_data[vector[i]] = NULL;  /*  ???   */
	}

	besta_handlers[vector[0]] = cwn_fd_intr;
	besta_handlers[vector[1]] = cwn_fd_intr;
	besta_handlers[vector[2]] = cwn_intr;
	besta_handlers[vector[3]] = cwn_intr;

	return;
}


static int do_cwnicmd (int target, char cmd[], int rw,
				void *addr, int len, int timeout) {
	volatile struct cwn_board *cwn = cwn_addr;
	volatile unsigned char *p = (unsigned char *) cwn->cmd_chain1;
	volatile unsigned char abort;
	int limit, moving_need = 0;

	if (addr) {
	    if (((int) addr ^ (int) cwn) & 0xfffe0000)  /*  not at board  */
		    moving_need = 1;
	    else
		addr = (void *) ((int) addr - (int) cwn);
	}

	if (moving_need && rw == 1)
		memcpy ((void *) cwn->iobuf1, addr, len);

	memcpy ((void *) (p + 1), cmd, 12);
	p[0] = ((char []) { 6, 10, 10, 12, 12, 12, 10, 10 })
						[(cmd[0] >> 5) & 0x7];
	cwn->cmd1.par0 = len;
	cwn->cmd1.par1 = (int) &((struct cwn_board *) 0)->cmd_chain1;
	cwn->cmd1.par2 = moving_need ? (int) &((struct cwn_board *) 0)->iobuf1
				     : (int) addr;
	cwn->cmd1.par3 = target;

	cwn->cmd1.cmd = 0x0060;     /*  TRSPMOD   */


	if (timeout <= 0)
	    while (!(cwn->cmd1.cmd & 0x8000)) ;
	else {
	    limit = jiffies + timeout;
	    while (!(cwn->cmd1.cmd & 0x8000) && jiffies < limit) ;
	    if (jiffies >= limit) {
		abort = cwn->intr[0].reg;   /*  SCSI reset, board reinit  */

		return SOFT_TIMEOUT;
	    }
	}


	switch (cwn->cmd1.cmd & 0xff) {
	    case 0x00:
		break;  /*  OK   */

	    case 0x01:
		return CTL_PARITY;  break;

	    case 0x06:
	    case 0x07:
		return CTL_ILL_PARAM;  break;

	    case 0xff:
		return CTL_ILL_CMD;  break;

	    case 0x0f:
		switch (cwn->cmd1.par0 & 0xff) {
		    case 0x01:  return CTL_TIMEOUT;  break;
		    case 0x03:  return CTL_READ_ERROR;  break;
		    case 0x04:  return CTL_WRITE_ERROR;  break;
		    case 0x06:  return CTL_RES_CONFL;  break;
		    default:  return TRY_FOR_SENSE;  break;
		}
		break;

	    default:
		return CTL_ERROR;
		break;
	}


	if (moving_need && rw == 0)
		memcpy (addr, (void *) cwn->iobuf1, len);

	return 0;
}

static int cwnicmd (int board, int target, char cmd[], int rw,
				void *addr, int len, int timeout) {
	volatile struct cwn_board *cwn = cwn_addr;
	int res;

	/*  Kludge, for cwn software bug : don`t allow
	  the `allocation length field' value more then needable.
	  This is very actual for INQUIRY and MODE SENSE command, which
	  are invoked by init phase. So, we must do some tests to determine
	  the `correct' length value.  Br-r-r-r...
	    Also I think about READ_BUFFER, WRITE_BUFFER, READ_DEFECT_DATA,
	  RECEIVE_DIAGNOSTIC and REQUEST_SENSE too...
	*/

	if (cmd[0] == INQUIRY || cmd[0] == MODE_SENSE) {
	    char new_cmd[12];
	    int new_len;

	    memcpy (new_cmd, cmd, sizeof (new_cmd));
	    /*  minimal for mode_sense, useful for inquiry   */
	    new_cmd[4] = 12;

	    res = do_cwnicmd (target, new_cmd, rw, (void *) cwn->iobuf1,
						    new_cmd[4], timeout);
	    if (res)  return res;

	    if (cmd[0] == INQUIRY)
		new_len = ((unsigned char *) cwn->iobuf1)[4] + 5;
	    else
		new_len = ((unsigned char *) cwn->iobuf1)[0] + 1;

	    if (new_len > len)  new_len = len;

	    new_cmd[4] = new_len;

	    return  do_cwnicmd (target, new_cmd, rw, addr, new_len, timeout);
	}

	/*  ordinary way for other commands...   */

	return  do_cwnicmd (target, cmd, rw, addr, len, timeout);
}


static int do_cwn_cmd (int board, int target, char cmd[], int rw,
						    void *addr, int len) {
	volatile struct cwn_board *cwn = cwn_addr;
	struct scsi_info_struct *scsi_info = cwn_info;
	unsigned short flags;

	save_flags (flags);
	cli();

	while (scsi_info[target].state != STATE_FREE)
	       sleep_on (&scsi_info[target].wait);

	scsi_info[target].state = STATE_CMD;

	cwn_chain[target].scsi_cmd[0] =
		((char []) { 6, 10, 10, 12, 12, 12, 10, 10 })
						[(cmd[0] >> 5) & 0x7];
	memcpy (&cwn_chain[target].scsi_cmd[1], cmd, 12);

	if (addr) {
	    if (rw == 1) {
		cwn_area[target].write_dirty = 0;
		memcpy ((char *) cwn + cwn_area[target].start_write, addr, len);
	    } else
		cwn_area[target].read_dirty = 0;
	}

	cwn_chain[target].cmd.par0 = len;
	cwn_chain[target].cmd.par1 =
			(int) &((struct cwn_board *) 0)->cmd_chain1;
	cwn_chain[target].cmd.par2 = (rw == 1) ? cwn_area[target].start_write
					       : cwn_area[target].start_read;
	cwn_chain[target].cmd.par3 = target;

	cwn_chain[target].cmd.cmd = 0x1060;     /*  TRSPMOD   */

	run_cwn_chain();

	sleep_on (&scsi_info[target].wait);

	restore_flags (flags);

	if (scsi_info[target].cmd_ret)
		return scsi_info[target].cmd_ret;

	if (addr && rw == 0)
		memcpy (addr, (char *) cwn + cwn_area[target].start_read, len);

	return 0;
}


static void do_cwn_request (int board, int target, int major) {
	volatile struct cwn_board *cwn = cwn_addr;
	struct scsi_info_struct *scsi_info = cwn_info;
	int dev, sect, num_sect, offset, left;
	struct buffer_head *bh;
	struct request *curr;
	struct hd_struct *part;
	unsigned char *p = cwn_chain[target].scsi_cmd;

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
	    end_request (0, 0, target, major);
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

	if (curr->cmd == READ) {

	    if ((offset = sect - cwn_area[target].read_sect) >= 0 &&
		(left = cwn_area[target].read_area - offset) > 0 &&
		cwn_area[target].read_dirty
	    ) {
		int length_to_movie = (num_sect > left ? left : num_sect) *
						    scsi_info[target].blksize;

		memcpy (curr->buffer,
			    (char *) cwn +
				(cwn_area[target].start_read +
				    offset * scsi_info[target].blksize),
					length_to_movie);

		/*  the same as good read intr...  */
		curr->nr_sectors -= length_to_movie >> 9;
		curr->current_nr_sectors -= length_to_movie >> 9;
		curr->sector += length_to_movie >> 9;
		curr->buffer += length_to_movie;

		if (curr->current_nr_sectors > 0)  goto repeat;
					/*  curr request yet not ended   */

		end_request (2, 0, target, major);
		goto repeat;

	    } else {
		/*  read several sectors to full all `read_area' blocks   */

		if (num_sect > cwn_area[target].read_area)
			panic ("Too mach num_sect when read\n");
		num_sect = cwn_area[target].read_area;

		cwn_area[target].read_dirty = 0;
		cwn_area[target].read_sect = sect;
	    }

	}
	else if (curr->cmd == WRITE) {

	    if (cwn_area[target].write_dirty &&
		sect == cwn_area[target].write_sect +
				cwn_area[target].write_head &&
		(left = cwn_area[target].write_count -
				cwn_area[target].write_head) > 0
	    ) {
		/*  the sector is already written...  */
		int sects_to_skip, length_to_skip;

		sects_to_skip = num_sect > left ? left : num_sect;
		length_to_skip = sects_to_skip * scsi_info[target].blksize;

		cwn_area[target].write_head += sects_to_skip;

		/*  the same as good write intr...  */
		curr->nr_sectors -= length_to_skip >> 9;
		curr->current_nr_sectors -= length_to_skip >> 9;
		curr->sector += length_to_skip >> 9;
		curr->buffer += length_to_skip;

		if (curr->current_nr_sectors > 0)  goto repeat;
					/*  curr request yet not ended   */

		end_request (2, 0, target, major);
		goto repeat;

	    } else {

		int blksize = scsi_info[target].blksize;
		char *ptr = (char *) cwn + cwn_area[target].start_write;
		struct request *tmp;
		struct buffer_head *bh;
		int length = num_sect * blksize;

		if (num_sect > cwn_area[target].write_area)
			panic ("Too mach num_sect when write\n");

		cwn_area[target].write_dirty = 0;
		cwn_area[target].write_sect = sect;
		cwn_area[target].write_head = num_sect;
		cwn_area[target].write_count = num_sect;

		memcpy (ptr, curr->buffer, length);
		ptr += length;


		/*  Get all the sequential `to write' blocks.  */

		tmp = curr;
		while (cwn_area[target].write_count <
			       cwn_area[target].write_area) {

		    if ((bh = tmp->bh) != NULL) {

			while ((bh = bh->b_reqnext) != NULL) {
			    int i = bh->b_size / blksize;
			    if (cwn_area[target].write_count + i >
				cwn_area[target].write_area
			    )  break;

			    memcpy (ptr, bh->b_data, bh->b_size);
			    ptr += bh->b_size;

			    cwn_area[target].write_count += i;
			}
		    }

		    tmp = tmp->next;

		    if (!tmp ||
			cwn_area[target].write_count >=
				cwn_area[target].write_area ||
			tmp->cmd != curr->cmd ||
			tmp->rq_dev != curr->rq_dev
		    )  break;

		    if (tmp->sector + tmp->nr_sectors > 2 * part->nr_sects)
			    break;      /*  error, leave for later...  */

		    sect = tmp->sector + 2 * part->start_sect;  /*  per 512b */
		    sect = (sect << 9) / blksize;

		    if (sect != cwn_area[target].write_sect +
				    cwn_area[target].write_count
		    )  break;   /*  non-sequential...  */


		    length = tmp->current_nr_sectors << 9;
		    num_sect = length / blksize;

		    if (cwn_area[target].write_count + num_sect >
					    cwn_area[target].write_area
		    )  break;

		    memcpy (ptr, tmp->buffer, length);
		    ptr += length;
		    cwn_area[target].write_count += num_sect;
		}

		/*  OK, write_area is fulled...  */

		sect = cwn_area[target].write_sect;
		num_sect = cwn_area[target].write_count;

		/*  check for read_area overloading...  */
		if (sect < cwn_area[target].read_sect +
			       cwn_area[target].read_area &&
		    sect + num_sect > cwn_area[target].read_sect
		)  cwn_area[target].read_dirty = 0;     /*  invalidate   */

	    }
	}
	else panic ("Bad request cmd\n");


	scsi_info[target].state = STATE_IO;

	cwn_chain[target].cmd.par0 = num_sect * scsi_info[target].blksize;
	cwn_chain[target].cmd.par1 =
			(int) &((struct cwn_board *) 0)->cmd_chain1;
	cwn_chain[target].cmd.par2 = (curr->cmd == READ)
					 ? cwn_area[target].start_read
					 : cwn_area[target].start_write;
	cwn_chain[target].cmd.par3 = target;

	if (sect > 0x1fffff || num_sect > 0xff) {
	    p[0] = 10;
	    p[1] = (curr->cmd == WRITE) ? WRITE_10 : READ_10;
	    p[2] = 0;
	    p[3] = (sect >> 24) & 0xff;
	    p[4] = (sect >> 16) & 0xff;
	    p[5] = (sect >> 8) & 0xff;
	    p[6] = sect & 0xff;
	    p[7] = 0;
	    p[8] = (num_sect >> 8) & 0xff;
	    p[9] = num_sect & 0xff;
	    p[10] = 0;

	} else {
	    p[0] = 6;
	    p[1] = (curr->cmd == WRITE) ? WRITE_6 : READ_6;
	    p[2] = (sect >> 16) & 0x1f;
	    p[3] = (sect >> 8) & 0xff;
	    p[4] = sect & 0xff;
	    p[5] = num_sect;
	    p[6] = 0;
	}

	cwn_chain[target].cmd.cmd = 0x1060;     /*  TRSPMOD   */

	run_cwn_chain();

	return;
}


static void end_request (int uptodate, int board, int target, int major) {
	volatile struct cwn_board *cwn = cwn_addr;
	struct request *curr = blk_dev[major].current_request;
	struct buffer_head *bh;


	if (!uptodate) {
	    /*  In our buffering scheme this mean that the sequential
	       read/write operation failed. Really only first some sectors
	       are needed, all another are additional. So, we do not
	       uptodate needable sectors and do not mark read/write_areas
	       as dirtied.
	    */

	    printk("end_request: %s error, dev %04lX, sector %lu\n",
			(curr->cmd == READ) ? "read" : "write",
			(unsigned long) curr->rq_dev, curr->sector >> 1);

	    curr->nr_sectors--;
	    curr->nr_sectors &= ~SECTOR_MASK;
	    curr->sector += (BLOCK_SIZE/512);
	    curr->sector &= ~SECTOR_MASK;
	}
	else if (uptodate == 2)
		uptodate = 1;      /*  found in buffer   */

	else if (curr->cmd == READ) {

	    /*  copy the needable data...  */
	    memcpy (curr->buffer, (char *) cwn + cwn_area[target].start_read,
						curr->current_nr_sectors << 9);

	    /*  additional sectors stored in the read_area buffer   */
	    cwn_area[target].read_dirty = 1;
	}
	else if (curr->cmd == WRITE)
	    /*  all write_area sectors are already written   */
	    cwn_area[target].write_dirty = 1;


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


static void request_sense (int target) {
	volatile struct cwn_board *cwn = cwn_addr;
	volatile unsigned char *p = (unsigned char *) cwn->cmd_chain1;

	p[0] = 6;
	memcpy ((void *) (p + 1), scsi_request_sense, 6);

	cwn->cmd1.par0 = sizeof (cwn_info[0].sense_buf);
	cwn->cmd1.par1 = (int) &((struct cwn_board *) 0)->cmd_chain1;
	cwn->cmd1.par2 = (int) &((struct cwn_board *) 0)->cmd_chain1[16];
	cwn->cmd1.par3 = target;

	cwn->cmd1.cmd = 0x1060;     /*  TRSPMOD   */

	return;
}

static void cwn_intr (int vec, void *data, struct pt_regs *fp) {
	volatile struct cwn_board *cwn = cwn_addr;
	struct scsi_info_struct *scsi_info = cwn_info;
	int err;
	int target = cwn_active_target;

	cwn_active_target = -1;     /*  let be free (be careful: reqsense!) */

	if (target < 0) {
	    cwn->cmd1.cmd = 0;

	    printk ("cwn: interrupt while inactive\n");

	    run_cwn_chain();
	    return;
	}

	if (scsi_info[target].state == STATE_FREE) {
	    cwn->cmd1.cmd = 0;
	    cwn_chain[target].cmd.cmd = 0;

	    printk("%s: interrupt while inactive\n", name(target));

	    run_cwn_chain();
	    return;
	}

	err = 0;

	if (scsi_info[target].req_sense) {

	    if (cwn->cmd1.cmd & 0xff) {
		scsi_info[target].req_sense = 0;
		printk ("%s: Bad regsense (status = 0x%02x)\n", name(target),
							cwn->cmd1.cmd & 0xff);
		err = CTL_ERROR;
	    } else
		memcpy (scsi_info[target].sense_buf,
			    (void *) &cwn->cmd_chain1[16],
				sizeof (scsi_info[target].sense_buf));

	} else {

	    switch (cwn->cmd1.cmd & 0xff) {

		case 0x00:      /*  OK   */
		    break;

		case 0x01:
		    printk ("%s: SCSI parity error\n", name(target));
		    err = CTL_ERROR;
		    break;

		case 0x06:
		    printk ("%s: invalid parameter\n", name(target));
		    err = CTL_ILL_PARAM;
		    break;

		case 0x07:
		    printk ("%s: illegal use of command\n", name(target));
		    err = CTL_ILL_PARAM;
		    break;

		case 0x0b:
		    printk ("%s: address error\n", name(target));
		    err = CTL_ERROR;
		    break;

		case 0x0c:
		    printk ("%s: on-board software exception\n", name(target));
		    err = CTL_ERROR;
		    break;

		case 0x0e:
		    printk ("%s: compare failure\n", name(target));
		    err = CTL_ERROR;    /*  ????   */
		    break;

		case 0xff:
		    printk ("%s: illegal command\n", name(target));
		    err = CTL_ILL_CMD;
		    break;

		case 0x0f:

		    switch (cwn->cmd1.par0 & 0xff) {
			case 0x01:
			    printk ("%s: Timeout\n", name(target));
			    err = CTL_TIMEOUT;
			    break;

			case 0x03:
			    printk ("%s: SCSI bus read error\n", name(target));
			    err = CTL_READ_ERROR;
			    break;

			case 0x04:
			    printk ("%s: SCSI bus write error\n", name(target));
			    err = CTL_WRITE_ERROR;
			    break;

			case 0x06:
			    printk ("%s: SCSI res. conflict\n", name(target));
			    err = CTL_RES_CONFL;
			    break;

			default:
			    cwn_chain[target].cmd.cmd = 0;
			    request_sense (target);
			    scsi_info[target].req_sense = 1;
			    cwn_active_target = target;     /*  the same...  */

			    return;
			    break;
		    }
		    break;

		default:
		    printk ("%s: Unknown error 0x%02x\n", name(target),
							cwn->cmd1.cmd & 0xff);
		    err = CTL_ERROR;
		    break;
	    }
	}

#if 1
	if (err) {
	    struct cwn_chain *ch = &cwn_chain[target];
	    unsigned char *p = cwn_chain[target].scsi_cmd;
	    int i;

	    printk ("(cmd=%x par0=%x par1=%lx par2=%lx par3=%x\n",
			ch->cmd.cmd, ch->cmd.par0, ch->cmd.par1,
			ch->cmd.par2, ch->cmd.par3);
	    printk (" scsi_cmd = ");
	    for (i = 0; i < 13; i++)  printk ("%x, ", *p++);
	    printk (")\n");
	}
#endif

	cwn->cmd1.cmd = 0;
	cwn_chain[target].cmd.cmd = 0;

	if (!scsi_info[target].inthandler) {

		printk("%s: interrupt from unused target\n", name(target));
		scsi_info[target].state = STATE_FREE;   /* avoid other  */

		run_cwn_chain();
		return;
	}

	scsi_info[target].inthandler (target, err, scsi_info);

	run_cwn_chain();

	return;
}


static void run_cwn_chain (void) {
	volatile struct cwn_board *cwn = cwn_addr;
	int target;

	if (cwn_active_target >= 0)  return;    /*  ctl is busy   */


	for (target = 0; target < MAX_DEV; target++)
		if (cwn_chain[target].cmd.cmd)  break;

	if (target == MAX_DEV)  return;

	cwn_active_target = target;

	cwn->cmd1.par0 = cwn_chain[target].cmd.par0;
	cwn->cmd1.par1 = cwn_chain[target].cmd.par1;
	cwn->cmd1.par2 = cwn_chain[target].cmd.par2;
	cwn->cmd1.par3 = cwn_chain[target].cmd.par3;

	memcpy ((void *) cwn->cmd_chain1, cwn_chain[target].scsi_cmd,
				    sizeof (cwn_chain[target].scsi_cmd));

	cwn->cmd1.cmd = cwn_chain[target].cmd.cmd | 0x1000;

	return;
}


/*      CWN  FLOPPY  STUFF        */

static int cwn_fd_open (struct inode *inode, struct file *filp) {
	int dev = MINOR (inode->i_rdev);
	int err, i;

	if (dev >= FD_MINORS)  return -ENXIO;
	if (dev && cwn_fd_par[dev].heads == 0)  return -ENXIO;

/*      if (filp && filp->f_mode)
		check_disk_change (inode->i_rdev);      */

	if (fd_access_count > 0) {
		fd_access_count++;
		return 0;
	}

	if (dev == 0) {     /*  autoprobing is needable   */
	    static int last_probing = 0;

	    /*  There are some reasons to probe all list each time,
		because formats PC720 & PC360 are very similar, etc.
		But there may be very intensive  `open/close'ing
		at the same small time period (i.e. -- `mount -t auto')
		For such cases we try to use last found format the first,
		while 4 seconds` time interval is not expired.
	    */

	    if (jiffies > last_probing + 4*HZ  ||
		(fd_real_dev <= 0 && fd_real_dev >= FD_MINORS)
	    )  fd_real_dev = FIRST_REAL_DEV;

	    fd_in_probe = 1;    /*  don`t error reports to console...   */

	    if (do_fd_cmd (fd_real_dev, 67) < 0 ||
		do_fd_cmd (fd_real_dev, 321) < 0
	    ) {   /*  Not the same as previous, try again all the list   */

		for (i = FIRST_REAL_DEV; i < FD_MINORS; i++) {
		    if (i == fd_real_dev)  continue;   /*  has been probed */
		    if (!cwn_fd_par[i].heads)  continue;    /*  not supp  */

		    if (do_fd_cmd (i, 67) >= 0 &&
			do_fd_cmd (i, 321) >= 0
		    )  break;
		}
		if (i == FD_MINORS) {
			fd_in_probe = 0;
			return -ENXIO;
		}

		fd_real_dev = i;
		last_probing = jiffies;
	    }

	    fd_in_probe = 0;

	    /*  set true block size for autoprobed dev...  */
	    cwn_fd_blocksizes[0] = cwn_fd_blocksizes[fd_real_dev];

	    fd_media_tested = 1;        /*  read probe was done    */
	    fd_access_count++;
	    return 0;
	}

	/* usual case  */
	err = do_fd_cmd (dev, 67);
	if (err)  return err;

	fd_real_dev = dev;

	fd_access_count++;
	fd_media_tested = 0;    /*  yet no read probing...   */
	return 0;
}

static void cwn_fd_release (struct inode *inode, struct file *file) {

	fsync_dev(inode->i_rdev);

	fd_access_count--;
	if (fd_access_count == 0) {

	    invalidate_inodes (inode->i_rdev);
	    invalidate_buffers (inode->i_rdev);

	    fd_media_tested = 0;
	}

	return;
}


static void do_fd_request (void) {
	volatile struct cwn_board *cwn = (struct cwn_board *) cwn_addr;
	int dev, sect, num_sects;
	struct buffer_head *bh;
	struct request *curr;

repeat:
	curr = blk_dev[CWN_FD_MAJOR].current_request;

	if (!curr || curr->rq_status == RQ_INACTIVE)  return;

	if (MAJOR(curr->rq_dev) != CWN_FD_MAJOR)
		panic ("cwn_fd: request list destroyed !  ");

	bh = curr->bh;
	if (!bh || !buffer_locked (bh))
		panic ("cwn_fd: block not locked !  ");

	dev = MINOR (curr->rq_dev);
	if (dev == 0)  dev = fd_real_dev;      /*  autoprobed dev   */

	if (dev >= FD_MINORS ||
	    cwn_fd_par[dev].heads == 0 ||
	    dev != fd_real_dev ||
	    curr->sector + curr->nr_sectors > 2*cwn_fd_part[dev].nr_sects
	) {
		fd_end_request (0, CWN_FD_MAJOR);
		goto repeat;
	}

	if (cwn_fd_busy)  return;


	sect = ((curr->sector +
		    2 * cwn_fd_part[dev].start_sect) <<
			(3 - cwn_fd_par[dev].bs_bits)) >> 1;

	num_sects = (curr->current_nr_sectors <<
			(3 - cwn_fd_par[dev].bs_bits)) >> 1;

	if (curr->cmd == WRITE)
		memcpy ((void *) cwn->iobuf0, curr->buffer,
					curr->current_nr_sectors << 9);
	else if (!fd_media_tested) {
	    short *p = (short *) &cwn->iobuf0;
	    short i;

	    /*  see the comment in `do_fd_cmd()'...   */
	    for (i = 0; i < 512; i++)  *p++ = i;
	}

	cwn->cmd0.par0 = num_sects;
	cwn->cmd0.par1 = sect;
	cwn->cmd0.par2 = (int) &((struct cwn_board *) 0)->iobuf0;
	cwn->cmd0.par3 = 3;     /*  hardcoded ???   */

	if (curr->cmd == READ)
		cwn->cmd0.cmd = 0x1e20;     /*  GETBLOC   */
	else if (curr->cmd == WRITE)
		cwn->cmd0.cmd = 0x1e22;     /*  PUTBLOC   */
	else  panic ("cwn_fd: unknown command");

	cwn_fd_busy = FD_STATE_IO;

	return;
}

static void fd_end_request (int uptodate, int major) {
	volatile struct cwn_board *cwn = (struct cwn_board *) cwn_addr;
	struct request *curr = blk_dev[major].current_request;
	struct buffer_head *bh;

	curr->errors = 0;

	if (!fd_media_tested && curr->cmd == READ) {
	    short *p = (short *) &cwn->iobuf0;
	    short i;

	    for (i = 0; i < 512; i++)
		if (*p++ != i)  break;

	    if (i == 512)  uptodate = 0;
	    else  fd_media_tested = 1;
	}

	if (!uptodate) {
	    printk("end_request: I/O error, dev %04lX, sector %lu\n",
			(unsigned long) curr->rq_dev, curr->sector >> 1);

	    curr->nr_sectors--;
	    curr->nr_sectors &= ~SECTOR_MASK;
	    curr->sector += (BLOCK_SIZE/512);
	    curr->sector &= ~SECTOR_MASK;
	}
	else if (curr->cmd == READ)
		memcpy (curr->buffer, (char *) &cwn->iobuf0,
					    curr->current_nr_sectors << 9);

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


static void cwn_fd_intr (int vec, void *data, struct pt_regs *fp) {
	volatile struct cwn_board *cwn = (struct cwn_board *) cwn_addr;
	struct request *curr;
	int err = 1;

	if (cwn_fd_busy == FD_STATE_FREE) {
		printk ("cwn_fd: interrupt while inactive\n");
		cwn->cmd0.cmd = 0;
		return;
	}

	switch (cwn->cmd0.cmd & 0xff) {

	    case 0x00:      /*  OK   */
		err = 0;
		break;

	    case 0x01:
		printk ("cwn_fd: SCSI parity error\n");
		break;

	    case 0x06:
		printk ("cwn_fd: invalid parameter\n");
		break;

	    case 0x07:
		printk ("cwn_fd: illegal use of command\n");
		break;

	    case 0x0b:
		printk ("cwn_fd: address error\n");
		break;

	    case 0x0c:
		printk ("cwn_fd: on-board software exception\n");
		break;

	    case 0x0e:
		printk ("cwn_fd: compare failure\n");
		break;

	    case 0xff:
		printk ("cwn_fd: illegal command\n");
		break;

	    case 0x0f:

		switch (cwn->cmd0.par0 & 0xff) {
		    case 0x01:
			if (!fd_in_probe)
				printk ("cwn_fd: Timeout\n");
			break;

		    case 0x02:
			printk ("cwn_fd: Floppy not ready\n");
			break;

		    case 0x03:
			if (!fd_in_probe)
				printk ("cwn_fd: FDC read error\n");
			break;

		    case 0x04:
			printk ("cwn_fd: FDC write error\n");
			break;

		    case 0x05:
			if (!fd_in_probe)
				printk ("cwn_fd: record not found\n");
			break;

		    case 0x06:
			if (!fd_in_probe)
				printk ("cwn_fd: format error\n");
			break;

		    default:
			printk ("cwn_fd: unknown add error code 0x%04x\n",
							cwn->cmd0.par0);
			break;
		}
		break;

	    default:
		printk ("cwn_fd: Unknown error 0x%02x\n",
					    cwn->cmd0.cmd & 0xff);
		break;
	}

	cwn->cmd0.cmd = 0;

	if (cwn_fd_busy == FD_STATE_IO) {

	    if (err)
		fd_end_request (0, CWN_FD_MAJOR);
	    else {
		curr = blk_dev[CWN_FD_MAJOR].current_request;

		curr->nr_sectors -= curr->current_nr_sectors;
		curr->sector += curr->current_nr_sectors;

		if (curr->bh || curr->nr_sectors <= 0)
			fd_end_request (1, CWN_FD_MAJOR);
	    }

	    cwn_fd_busy = FD_STATE_FREE;

	}
	else if (cwn_fd_busy == FD_STATE_CMD) {

	    if (err)  fd_cmd_ret = -EIO;
	    else  fd_cmd_ret = 0;

	    cwn_fd_busy = FD_STATE_FREE;

	}
	else if (cwn_fd_busy == FD_STATE_BREAK) {
	    cwn_fd_busy = FD_STATE_FREE;
	}
	else panic ("Bad cwn_fd_busy type ");

	if (waitqueue_active (&fd_cmd_wait))
		wake_up_interruptible (&fd_cmd_wait);

	/*  always run request queue   */
	do_fd_request ();

	return;
}


static int do_fd_cmd (int dev, int cmd) {
	volatile struct cwn_board *cwn = (struct cwn_board *) cwn_addr;
	unsigned short flags;

	if (!dev)  return -EINVAL;

	save_flags (flags);
	cli();

	while (cwn_fd_busy != FD_STATE_FREE) {
	    interruptible_sleep_on (&fd_cmd_wait);

	    if (current->signal & ~current->blocked) {
		    restore_flags (flags);
		    return -ERESTARTSYS;
	    }
	}
	cwn_fd_busy = FD_STATE_CMD;


	if (cmd == 67) {        /*  initialization   */

	    memcpy ((void *) cwn->iobuf0, &cwn_fd_par[dev],
					    sizeof (cwn_fd_par[dev]));

	    cwn->cmd0.par1 = (int) &((struct cwn_board *) 0)->iobuf0;
	    cwn->cmd0.par3 = 3;     /*  hardcoded ???  */

	    cwn->cmd0.cmd = 0x1e0a;     /*  SUNPARM   */

	}
	else if (cmd == 321) {   /*  read probe   */
	    short *p = (short *) &cwn->iobuf0;
	    short i;

	    /*  cwn software sometimes returns success for read block cmd,
	      in a case there is no floppy in the drive. So, we should do
	      any test to decide is this returned success true or it is a bug.
		To do this, we full `iobuf0' area by test data and then
	      check, are this data saved or rewritten by floppy.
	      If this is the same data as we wrote, read probe is failed.
		We use incremented short words to generate test data,
	      assuming this is never match a floppy null block...
	    */
	    for (i = 0; i < 512; i++)  *p++ = i;

	    cwn->cmd0.par0 = 1;
	    cwn->cmd0.par1 = 0;
	    cwn->cmd0.par2 = (int) &((struct cwn_board *) 0)->iobuf0;
	    cwn->cmd0.par3 = 3;     /*  hardcoded ???   */

	    cwn->cmd0.cmd = 0x1e20;     /*  GETBLOC   */
	}
	else if (cmd == 322) {   /*  format   */

	    cwn->cmd0.par3 = 3;     /*  hardcoded ???  */

	    cwn->cmd0.cmd = 0x1e57;     /*  FORMAT   */
	}
	else {
	    cwn_fd_busy = FD_STATE_FREE;
	    restore_flags (flags);
	    printk ("cwn_fd: unknown special cmd %d\n",cmd);
	    return  -ENOSYS;
	}

	interruptible_sleep_on (&fd_cmd_wait);
	if (cwn_fd_busy != FD_STATE_FREE) {
		cwn_fd_busy = FD_STATE_BREAK;
		restore_flags (flags);
		return -EINTR;
	}

	if (cmd == 321 && fd_cmd_ret == 0) {
	    short *p = (short *) &cwn->iobuf0;
	    short i;

	    for (i = 0; i < 512; i++)
		if (*p++ != i)  break;

	    if (i == 512)  fd_cmd_ret = -ENXIO;
	}

	restore_flags (flags);

	return fd_cmd_ret;
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

static int cwn_fd_ioctl (struct inode *inode, struct file *file,
			      unsigned int cmd, unsigned long arg) {
	int dev, err;

	dev = MINOR(inode->i_rdev);
	if (dev == 0)  dev = fd_real_dev;      /*  autoprobed dev   */

	switch (cmd) {

	    case BLKGETSIZE:   /* Return device size */
		if (!arg)  return -EINVAL;
		err = verify_area (VERIFY_WRITE, (long *) arg, sizeof(long));
		if (err)
			return err;
		put_fs_long (cwn_fd_part[dev].nr_sects << 1, (long *) arg);
		return 0;

	    case SIOCFORMAT:  /*  format diskette (svr3)  */
		err = do_fd_cmd (dev, 322);
		return err;

	    case SIOCGETP:    /*  get parameters (svr3)  */
		{   struct dkinfo dkinfo;

		    err = verify_area (VERIFY_WRITE, (void *) arg,
							sizeof (dkinfo));
		    if (err)  return err;

		    dkinfo.vol.unit = 7;
		    dkinfo.vol.disktype = dev;
		    dkinfo.vol.sec_p_track =
			cwn_fd_par[dev].sects_per_cyl / cwn_fd_par[dev].heads;
		    dkinfo.vol.hd_offset = 0;
		    dkinfo.vol.hd_p_vol = cwn_fd_par[dev].heads;
		    dkinfo.vol.cyl_p_vol = cwn_fd_par[dev].cylinders;
		    dkinfo.vol.steprate = cwn_fd_par[dev].steprate;
		    dkinfo.vol.interleave = cwn_fd_par[dev].table_num;
		    dkinfo.vol.bias_trtr = 0;
		    dkinfo.vol.bias_hdhd = 0;
		    dkinfo.ldev.bl_offset = cwn_fd_part[dev].start_sect;
		    dkinfo.ldev.bl_size = cwn_fd_part[dev].nr_sects;

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

