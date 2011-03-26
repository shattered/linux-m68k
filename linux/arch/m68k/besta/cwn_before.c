/*
 *	Experimental !!!  Not ended !!!
 * besta/cwn_before.c -- Routines to initialize and load local software
 *			 onto CWN board.
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

extern int cwn_soft_start, cwn_soft_end;    /*  local software binary...  */
extern int cwn_loader_start, cwn_loader_end;    /*  load insns binary...  */
extern void cwn_after_init (struct VME_board *, int []);    /*  real init... */

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


/*  This routine only checks the board, get interrupt vectors
   and load a local software. After local software has been loaded,
   a real init routine (cwn_after_init)  will be called.
*/
void cwn_init (struct VME_board *VME, int on_off) {
	volatile struct cwn_board *cwn = (struct cwn_board *) VME->addr;
	volatile unsigned char reset;
	int i;
	int vector[4];
	typeof (jiffies) limit;

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

	printk ("CWN v=%x, ser=%x", cwn->version >> 8,
					    cwn->version & 0xff);

	/*  maininit cmd for board   */

	memcpy ((void *) cwn->iobuf1, ((char []) { 0x7, 0x04, 0 }), 3);
	cwn->cmd1.par1 = (int) &((struct cwn_board *) 0)->iobuf1;
	cwn->cmd1.cmd = 0x000d;     /*  MAININIT   */
	while (!(cwn->cmd1.cmd & 0x8000)) ;


	/*  do not want any hashing...  */
	for (i = 0; i < 7; i++) {

	    cwn->cmd1.par1 = (int) &((struct cwn_board *) 0)->iobuf1;
	    memset ((void *) cwn->iobuf1, 0, 12);
	    cwn->cmd1.par3 = 0;

	    cwn->cmd1.cmd = (i << 9) | 0x000a;     /*  SUNPARM   */
	    while (!(cwn->cmd1.cmd & 0x8000)) ;
	}

	cwn->cmd1.cmd = 0x005f;     /*  CLRHASH   */
	while (!(cwn->cmd1.cmd & 0x8000)) ;

	/*  OK, hear we can assume all cwn->memory area is free...  */

	printk (" : Loading local software...");

	/*  Load a local software...   */
	memcpy ((void *) cwn->memory, &cwn_soft_start,
				(int) &cwn_soft_end - (int) &cwn_soft_start);
	/*  Load a local software booter...  */
	memcpy ((void *) cwn->iobuf1, &cwn_loader_start,
			    (int) &cwn_loader_end - (int) &cwn_loader_start);

	cwn->cmd1.par1 = (int) &((struct cwn_board *) 0)->iobuf1;
	cwn->cmd1.cmd = 0x0050;     /*  EXPROG   */

	/*  After local software has been loaded, loader should emulate
	   the ordinary software way to indicate `command complete'.
	    So, we can decide success/failure by a one common checking.
	*/
	limit = jiffies + 4 * HZ;
	while (!(cwn->cmd1.cmd & 0x8000) && jiffies < limit) ;

	if (jiffies >= limit ||
	    cwn->cmd1.cmd != 0x8000
	) {
	    printk ("FAILED! (ignore board)\n");

	    /*  reset the cwn board for paranoidal reasons...  */
	    reset = cwn->reset[0].reg;

	    VME->present = 0;   /*  Let My People Go! (because resetted)  */
	    return;
	}

	/*  Success:  go to the new code...  */

	/*  vectors are generated and checked  */
	cwn_after_init (VME, vector);

	return;
}

/*  load insns binary code...   */

__asm ("
text
cwn_loader_start:
	mov.w   &0x2700,%sr      |  set intr off

	lea.l   0x0000,%a0
	mov.l   &cwn_soft_code_start,%d0
	lsr.l   &2,%d0
	sub.l   &1,%d0
1:      clr.l   (%a0)+
	dbra    %d0,1b

	lea.l   0x4000,%a0      |  offset of `cwn->memory'
	lea.l   cwn_soft_code_start,%a1
	mov.l   &0x2000,%d0
	sub.l   %a1,%d0
	lsr.l   &2,%d0
	sub.l   &1,%d0
1:      mov.l   (%a0)+,(%a1)+
	dbra    %d0,1b

	mov.w   &0x8000,0x2100  |  to emulate success in old software style
	mov.w   &0x8000,0x3500  |  to emulate success in old software style
	mov.w   &0x0000,0x2000  |  to indicate initial busy state...

	mov.l   &cwn_soft_ini_stack,%sp
	jmp     cwn_soft_entry
	nop
	nop
	nop
cwn_loader_end:
");



#if 0
/*  Put it into the better place...   */

/*  128 / 8 == 16   */
#define MASK_SIZE       16
unsigned char cwn_mask[MASK_SIZE] = {
	0x00, 0x7f, [ 2 ... 15 ] = 0xff
};

unsigned int cwn_alloc (unsigned int blocks) {
	register unsigned long start_block = 8;
	register unsigned long bits, n;

	if (blocks > 32)  return 0;

	do {
	    __asm volatile ("bfffo  (%3) {%0:&0},%0\n\t"
			    "bfexts (%3) {%0:%4},%1"
			    : "=d" (start_block), "=d" (bits)
			    : "0" (start_block), "a" (cwn_mask), "d" (blocks)
	    );

	    if ((bits + 1) == 0) {    /*  found   */
		__asm ("bfclr  (%0) {%1:%2}"
		       :
		       : "a" (cwn_mask), "d" (start_block), "d" (blocks)
		       : "memory"
		);

		return  start_block << 10;
	    }

	    if (blocks == 1) {
		start_block++;
		continue;
	    }

	    /*  seek `start_block' to position after last `0' in
	       already readed `bits'...
	    */
	    bits = ~bits;
	    bits = bits & (-bits);

	    __asm ("bfffo  %1 {&0:&0},%0"
		   : "=d" (n)
		   : "d" (bits)
	    );
	    start_block += (blocks - 31 + n);

	} while (start_block < 128);

	return 0;
}

void cwn_free (unsigned int addr, unsigned int blocks) {
	register unsigned long start_block = addr >> 10;

	if (start_block >= 128)  return;

	__asm ("bfclr  (%0) {%1,%2}"
	       :
	       : "a" (cwn_mask), "d" (start_block), "d" (blocks)
	       : "memory"
	);

	return;
}

#endif
