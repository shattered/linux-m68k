/*
 * besta/xclk.c -- Clock and other HCPU30-relative routines.
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
#include <linux/kd.h>
#include <linux/sched.h>
#include <linux/errno.h>
#include <linux/tty.h>
#include <linux/console.h>
#include <linux/termios.h>
#include <linux/interrupt.h>
#include <linux/serial.h>
#include <linux/linkage.h>
#include <linux/kernel_stat.h>

#include <asm/system.h>
#include <asm/pgtable.h>
#include <asm/traps.h>
#include <asm/irq.h>

#include "besta.h"
#include "hcpu30.h"

struct xclk {
	char            x0;
	char            r1[3];
	unsigned char   x4;
	unsigned char   x5;
	char            r6[2];
	unsigned char   x8;
	unsigned char   x9;
	unsigned char   xa;
	unsigned char   xb;
	unsigned char   xc;
	unsigned char   xd;
	char            re[2];
	char            r10[16];
};

void hcpu30_gettod (int *year, int *month, int *day,
				int *hour, int *min, int *sec) {
	volatile struct xclk *v = (struct xclk *) X_ADDR;

	while (v->x0 >= 0);
	v->x0 = 3;
	while (v->x0 >= 0);

	if ((unsigned char) v->x0 != 128 ||
	    v->x8 >= 60 || v->x9 >= 60 ||
	    v->xa >= 24 || v->xb > 31 || v->xc > 12) {
	    *year = *month = *day = *hour = *min = *sec = 0;
	    return;
	}

	*year = v->xd;
	*month = v->xc;
	*day = v->xb;
	*hour = v->xa;
	*min = v->x9;
	*sec = v->x8;

	return;
}

void hcpu30_reset (void) {
	volatile struct xclk *v = (struct xclk *) X_ADDR;
	int i;

	VME_deinit();

	i = 300000;
	while (i--) ;

	while (v->x0 >= 0);
	v->x0 = 1;

	while (1) ;
}

void hcpu30_sched_init (intfunc handler) {
	volatile struct xclk *v = (struct xclk *) X_ADDR;
	int vector, level;

	if (!besta_get_vect_lev ("xclk", &vector, &level)) {
	    vector = get_unused_vector();
	    level = XCLK_LEV;
	}
	else if (level < XCLK_LEV) {
	    printk ("Too low timer level %d. Used %d instead\n",
						    level, XCLK_LEV);
	    level = XCLK_LEV;
	}

	besta_handlers[vector] = handler;
	besta_intr_data[vector] = NULL;

	v->x5 = vector;
	v->x4 = level;
}


int hcpu30_set_clock_mmss (unsigned long time) {
	volatile struct xclk *v = (struct xclk *) X_ADDR;
	int sec = time % 60;
	int min = (time / 60) % 60;

	while (v->x0 >= 0);

	/*  get current hardware time   */
	v->x0 = 3;
	while (v->x0 >= 0);

	/*  set new sec and min   */
	v->x8 = sec;
	v->x9 = min;

	/*  set changed hardware time   */
	v->x0 = 2;

	return 0;
}

int hcpu30_hwclk (int rw, struct hwclk_time *time) {
	volatile struct xclk *v = (struct xclk *) X_ADDR;

	if (!rw) {      /*  read   */
	    while (v->x0 >= 0);

	    v->x0 = 3;
	    while (v->x0 >= 0);

	    if ((unsigned char) v->x0 != 128 ||
		v->x8 >= 60 || v->x9 >= 60 ||
		v->xa >= 24 || v->xb > 31 || v->xc > 12
	    )  return -EIO;

	    time->year = v->xd;
	    time->mon  = v->xc - 1;
	    time->day  = v->xb;
	    time->hour = v->xa;
	    time->min  = v->x9;
	    time->sec  = v->x8;
	    time->wday = -1;

	} else {        /*  write   */

	    v->x8 = time->sec;
	    v->x9 = time->min;
	    v->xa = time->hour;
	    v->xb = time->day;
	    v->xc = time->mon + 1;
	    v->xd = time->year;

	    while (v->x0 > 0);
	    v->x0 = 2;
	}

	return 0;
}


extern int _end;
#ifdef CONFIG_BLK_DEV_INITRD
extern int initrd_start, initrd_end;
#endif

void besta_bootinfo_init (char *bootstr) {
	volatile struct xclk *v = (struct xclk *) X_ADDR;
	struct hcpu30_info *hcpu30_info = (struct hcpu30_info *)
						&boot_info.bi_un.bi_hcpu30;
	unsigned int memory, start_ini, size;

	boot_info.machtype = MACH_BESTA_HCPU30;
	boot_info.cputype = CPU_68030 | FPU_68882;
	strncpy (boot_info.command_line, bootstr,
			    sizeof (boot_info.command_line));

	/*  get board info   */
	while (v->x0 > 0);
	do {
	    v->x0 = 4;
	    while (v->x0 > 0);
	} while (((unsigned char) v->x0) != 128);

	/*  save  dip switch  values   */
	hcpu30_info->dip_switch = v->x8 & 0xff;

	/*  memory initialize stuff   */
	memory = (v->x9 & 0xff) << 20;  /*  on-board memory size in bytes  */

	start_ini = (((int) &_end) + 3) & ~3;   /*  forward aligned `_end'  */

#ifdef CONFIG_BLK_DEV_INITRD
	/*  builting ramdisk stuff   */
	size = initrd_end - initrd_start;
	if (size < 4096) {
		initrd_start = 0;
		initrd_end = 0;
	}

	/*  if `initrd_start' is valid (i.e., != 0), then all possible
	   memory is already initialized by kernel preloader.
	    To determine memory high address it is enough to check
	   the area after `initrd_end' (kernel preloader don`t tell as
	    about this addr...)
	*/
	if (initrd_start)  start_ini = (initrd_end + 3) & ~3;
#endif

	memory = __rw_while_OK (start_ini, memory, PROBE_WRITE, 0,
						PORT_LONG, DIR_FORWARD);
	boot_info.memory[0].addr = 0;
	boot_info.memory[0].size = memory;     /*  size in bytes   */
	boot_info.num_memory = 1;       /*  one  block   */


	/*  stram size  checking...  (note: unsigned!)  */
	memory = __stram_check (0xffff9000, 0x00000000, 0x1000);

	hcpu30_info->stram_size = memory - 0xffff8000;

	/*  At this point check for any other present memory...  */
	__mem_collect (MAX_SCAN_ADDR, 512 * 1024);

#if 0
	{ struct besta_memory_info *ptr;
	  int num;
	  for (num = 0, ptr = besta_memory_info; ptr; num++, ptr = ptr->next) ;

	  besta_memory_info[num - 1].next = &besta_memory_info[num];
	  besta_memory_info[num].addr = 0xffff8000;
	  besta_memory_info[num].size = hcpu30_info->stram_size;
	  besta_memory_info[num].next = NULL;
	}
#endif

	return;
}

void besta_cache_init (void) {
	struct hcpu30_info *hcpu30_info = (struct hcpu30_info *)
						&boot_info.bi_un.bi_hcpu30;
	int cacr;

	if (hcpu30_info->dip_switch & 0x40)  cacr = 0x1111;
	else  cacr = 0x0111;

	cacr |= 0x0808;
	__asm volatile ("mov.l  %0,%%cacr" : : "r" (cacr));

	return;
}
