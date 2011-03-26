/*
 * besta/cp31.c -- Main source file for CP20/CP30/CP31 configurations.
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
#include <linux/binfmts.h>
#include <linux/netdevice.h>

#include <asm/setup.h>
#include <asm/machdep.h>
#include <asm/system.h>
#include <asm/pgtable.h>
#include <asm/traps.h>
#include <asm/irq.h>

#include "besta.h"
#include "cp31.h"


/*      Machine-depended routines.       */

/*    `To make Linux happy' routines.  */

static int cp31_make_Linux_happy_ENXIO (void) {  return -ENXIO; }
static void cp31_make_Linux_happy_void (void) {}
static void *cp31_make_Linux_happy_NULL (void) {  return NULL; }
static int cp31_make_Linux_happy_0 (void) {  return 0; }

static unsigned long cp31_gettimeoffset (void) {
	return 0;   /*  don`t know how to read hardware timer   */
}

static void cp31_process_int (int vec, struct pt_regs *fp) {
	ushort sr;
	intfunc handler;
	void *data;

	asm volatile ("movew %/sr,%0" : "=dm" (sr));

	kstat.interrupts[(sr >> 8) & 7]++;

	handler = besta_handlers[vec];
	data = besta_intr_data[vec];

	if (handler == NULL)  return;

	handler (vec, data, fp);

	return;
}


/*  This is needable because mc68230 pit timer
  wants to be resetted on interrupt.
*/
static void cp31_clk_intr (int vec, void *data, struct pt_regs *fp) {
	volatile struct pit *pit = (struct pit *) PIT_ADDR;
	intfunc handler = data;

	pit->timer_status = 0x01;       /*  reset interrupt status   */
	pit->timer_cntl = 0xe1;         /*  set timer again   */

	handler (vec, NULL, fp);
}

static void cp31_sched_init (intfunc handler) {
	volatile struct pit *pit = (struct pit *) PIT_ADDR;
	volatile struct bim *bim = (struct bim *) BIM_ADDR;
	int vector, level;

	if (!besta_get_vect_lev ("clk", &vector, &level)) {
	    vector = get_unused_vector();
	    level = CLK_LEV;
	}
	else if (level < CLK_LEV) {
	    printk ("Too low timer level %d. Used %d instead\n",
						    level, CLK_LEV);
	    level = CLK_LEV;
	}

	/*  initialize pit mc68230 port...  */

	pit->gen_cntl = 0x30;   /*  mode 0, H1234 ena, inverted sense   */
	pit->serv_req = 0x08;   /*  pc4 - func, pc5 - irq, pc6 - func   */
	pit->a_cntl = 0x80;     /*  submode 1x, h2 dis   */
	pit->a_dir = 0x00;      /*  port A : all input  (dip_switch bits)   */
	pit->b_cntl = 0x84;     /*  submode 1x, h4 input status, h4 ena   */
	pit->b_data = 0xff;
	pit->b_dir = 0xff;      /*  port B : all output   */
	pit->c_data = 0x86;
	pit->c_dir = 0x87;      /*  port C : input and output */
	pit->status = 0x0f;     /*  reset pending interrupts   */

	/*  initialize the timer interrupts  */
	bim->vect[2].reg = vector;
	bim->cntrl[2].reg = level | 0x10;

	/*  set timer stuff   */
	pit->timer_start0 = ((8064000/32/HZ + 1) >> 24) & 0xff;
	pit->timer_start1 = ((8064000/32/HZ + 1) >> 16) & 0xff;
	pit->timer_start2 = ((8064000/32/HZ + 1) >> 8) & 0xff;
	pit->timer_start3 = (8064000/32/HZ + 1) & 0xff;
	pit->timer_cntl = 0xe1;

	besta_handlers[vector] = cp31_clk_intr;
	besta_intr_data[vector] = handler;
}

static void cp31_gettod (int *year, int *month, int *day,
				int *hour, int *min, int *sec) {
	struct hwclk_time time;

	memset (&time, 0, sizeof (time));

	if (besta_clock_base)
		besta_clock_base->hwclk (0, &time);

	*year = time.year;
	*month = time.mon + 1;
	*day = time.day;
	*hour = time.hour;
	*min = time.min;
	*sec = time.sec;

	return;
}

static int cp31_set_clock_mmss (unsigned long time) {

	if (!besta_clock_base)  return -1;

	return  besta_clock_base->set_clock_mmss (time);
}

static int cp31_hwclk (int rw, struct hwclk_time *time) {

	if (!besta_clock_base)  return -ENOSYS;

	return  besta_clock_base->hwclk (rw, time);
}

static void cp31_reset (void) {
	int i;

	VME_deinit();

	i = 300000;
	while (i--) ;

	i = 1;
	VME_probe ((void *) 0xfcfffffe, &i, PROBE_WRITE, PORT_BYTE);
	/*  what it was ???  */

	i = 0;
	__asm volatile ("mov.w &0x2700, %%sr\n\t"       /*  initial state   */
			"mov.l &0x1000, %%sp\n\t"       /*  initial stack   */
			"mov.l &0, %%d0\n\t"
			"mov.l %%d0, %%cacr\n\t"        /*  cache off   */
			"pmove (%0), %%tc\n\t"          /*  mmu off   */
			"jmp    0xff000008"             /*  to boot by PROM  */
			: : "a" (&i) : "d0", "memory"
	);

	/*  should not be reached   */
	while (1);

}

/*  cp31 should not use irq (non-vec) interrupts  */
static void cp31_irq_handler (int irq, void *dev_id, struct pt_regs *fp) {

	cp31_process_int (irq + VEC_SPUR, fp);

	return;
}

static void (*cp31_irq_handlers[SYS_IRQS])(int, void *, struct pt_regs *) = {
	cp31_irq_handler, cp31_irq_handler, cp31_irq_handler,
	cp31_irq_handler, cp31_irq_handler, cp31_irq_handler,
	cp31_irq_handler, cp31_irq_handler
};

static void cp31_debug_init (void) {

	/*  currently nothing to do, and may be the same in future...  */

	return;
}


void config_besta (void) {

	mach_sched_init = cp31_sched_init;
	mach_gettod = cp31_gettod;
	mach_set_clock_mmss = cp31_set_clock_mmss;
	mach_gettimeoffset = cp31_gettimeoffset;
	mach_hwclk = cp31_hwclk;
	mach_process_int = cp31_process_int;
	mach_reset = cp31_reset;
	mach_default_handler = &cp31_irq_handlers;
	mach_debug_init = cp31_debug_init;

	/*  cp31 don`t support DMA`s...  */
	mach_max_dma_address = 0;

/*  This pointers are not NULL-checked anyvere.   */
	mach_init_IRQ = (void *) cp31_make_Linux_happy_void;
	mach_mksound = (void *) cp31_make_Linux_happy_void;
	mach_video_setup = (void *) cp31_make_Linux_happy_void;
	mach_keyb_init = (void *) cp31_make_Linux_happy_ENXIO;
	mach_request_irq = (void *) cp31_make_Linux_happy_ENXIO;
	mach_free_irq = (void *) cp31_make_Linux_happy_ENXIO;
	mach_fb_init = (void *) cp31_make_Linux_happy_NULL;
	mach_get_irq_list = (void *) cp31_make_Linux_happy_0;

}


/*      Console init specific stuff       */

extern void register_console (void (*)(const char *));
extern void sio_putstring (const char *);

unsigned long besta_console_init (unsigned long mem_start,
					    unsigned long mem_end) {
	register_console (sio_putstring);

	return mem_start;
}


/*      Base devices init routine (i.e., motherboard etc.)      */

extern int cp31_sioinit (struct async_struct *, int);
extern int cp31_ceninit (void);
int cp31_rtcinit (void);

extern struct linux_binfmt svr3_format;

static void cp31_notify_board (void);


void besta_dev_init (void) {
	volatile struct pit *pit = (struct pit *) PIT_ADDR;
	struct cp31_info *cp31_info = (struct cp31_info *)
						&boot_info.bi_un.bi_hcpu30;

	/*  initialize xdus two ports   */
	cp31_info->num_sio = cp31_sioinit (rs_table, 0);    /*  1,2 or 3   */

	/*  initialize centronics port  */
	if (cp31_info->num_sio == 1)    /*  may be centronix on base board   */
		cp31_info->cen_present = cp31_ceninit();

	/*  initialize clock port   */
	cp31_info->rtc_present = cp31_rtcinit();

	printk ("SVR3.1/m68k binary compatibility "
		"code copyright 1996 Dm.K.Butskoy\n");
	register_binfmt (&svr3_format);


	/*  Here should be some memory testing to decide whether
	  we have a static or local ram, etc.
	*/

	if (cp31_info->rtc_present &&
	    cp31_info->cen_present &&
	    cp31_info->num_sio == 1
	)  cp31_info->board_type = CP31_BOARD_CP31;

	else if (!cp31_info->rtc_present &&
		 !cp31_info->cen_present &&
		 (boot_info.cputype & CPU_MASK) == CPU_68030
	)  cp31_info->board_type = CP31_BOARD_CP30;

	else if (!cp31_info->rtc_present &&
		 !cp31_info->cen_present &&
		 (boot_info.cputype & CPU_MASK) == CPU_68020
	)  cp31_info->board_type = CP31_BOARD_CP20;

	else
	   cp31_info->board_type = CP31_BOARD_UNKNOWN;

	if (cp31_info->board_type != CP31_BOARD_CP31 &&     /*  not CP31  */
	    besta_memory_info[0].size <= (4 * 1024 * 1024) &&
	    besta_memory_info[0].time < MIN_VME_TIME    /*  assume non-VME   */
	)  cp31_info->slave_stram_size = besta_memory_info[0].size;
	else  cp31_info->slave_stram_size = 0;

	cp31_info->dip_switch = pit->a_data ^ 0xff;

	/*  reset %cacr value if it is CP31 & LMx configuration...  */
	if (cp31_info->board_type == CP31_BOARD_CP31) {

	    /*  good value as default (`040, `060 way...)   */
	    if (besta_cacr_user < 0)  besta_cacr_user = 0x1111;

	    if (besta_cacr_user & ~0x3111)
		printk ("cacr=0x%04x: bad value\n", besta_cacr_user);
	    else {
		int cacr = besta_cacr_user;

		__asm volatile ("mov.l  %0,%%cacr" : : "r" (cacr));
	    }
	}

	cp31_notify_board();

	/*   Extra devices initializations (VME, peripherals, etc.).  */

	besta_drivers_init();

	VME_init();

	return;
}


static void cp31_notify_board (void) {
	struct cp31_info *cp31_info = (struct cp31_info *)
						&boot_info.bi_un.bi_hcpu30;
	char *name;
	unsigned int cacr;

	switch (cp31_info->board_type) {
	    case CP31_BOARD_CP20:
		name = cp31_info->slave_stram_size ? "CP20" : "CP20-1";  break;
	    case CP31_BOARD_CP30:
		name = cp31_info->slave_stram_size ? "CP30" : "CP30-1";  break;
	    case CP31_BOARD_CP31:  name = "CP31";  break;
	    default:  name = "unknown CPxx compatible";  break;
	}

	printk ("    BESTA  basic hardware found:   %s    board\n", name);

	printk ("(%s/%s, %d serials%s%s\n",
		(boot_info.cputype & CPU_68030) ? "mc68030" : "mc68020",
		(boot_info.cputype & FPU_68882) ? "mc68882" : "mc68881",
		cp31_info->num_sio,
		cp31_info->cen_present ? ", centronix" : "",
		cp31_info->rtc_present ? ", clock" : "");

	__asm volatile ("mov.l %%cacr, %0" : "=da" (cacr) : );

	printk ("VME: master ROR 32bit, data burst %s, "
		"cacr=0x%04x, dip switch=0x%02lx, %ldk stram)\n",
		(cacr & 0x1000) ? "on" : "off",
		cacr, cp31_info->dip_switch,
		cp31_info->stram_size >> 10);

	if (cp31_info->slave_stram_size)
	    printk ("  with SP500 slave board, %ldk static RAM\n",
				    cp31_info->slave_stram_size >> 10);

	if (cp31_info->board_type == CP31_BOARD_CP31 &&
	    besta_memory_info[0].time < MIN_VME_TIME &&
	    (besta_memory_info[0].flags & CAN_BURST)
	)  printk ("  with LMx board(s), %dMb dinamic burstable RAM\n",
					besta_memory_info[0].size >> 20);
	return;
}

void besta_get_model (char *model) {
	struct cp31_info *cp31_info = (struct cp31_info *)
						&boot_info.bi_un.bi_hcpu30;
	char *name;

	switch (cp31_info->board_type) {
	    case CP31_BOARD_CP20:
		name = cp31_info->slave_stram_size ? "CP20" : "CP20-1";  break;
	    case CP31_BOARD_CP30:
		name = cp31_info->slave_stram_size ? "CP30" : "CP30-1";  break;
	    case CP31_BOARD_CP31:  name = "CP31";  break;
	    default:  name = "unknown CPxx compatible";  break;
	}

	sprintf (model, "%s VME board", name);

	return;
}

int besta_get_hardware_list (char *buffer) {

	/*  currently nothing   */

	return 0;
}


/*   boot stuff   */

extern int _end;
#ifdef CONFIG_BLK_DEV_INITRD
extern int initrd_start, initrd_end;
#endif

void besta_bootinfo_init (char *bootstr) {
	struct cp31_info *cp31_info = (struct cp31_info *)
						&boot_info.bi_un.bi_hcpu30;
	volatile struct pit *pit = (struct pit *) SECRET_ADDR;
	volatile struct bim *bim = (struct bim *) BIM_ADDR;
	unsigned int memory, start_ini, size;
	int i;

	/*  paranoia...  */
	for (i = 0; i < 4; i++) {
		bim->cntrl[i].reg = 0;
		bim->vect[i].reg = 0x0f;
	}

	/*  Let they think I know nothing about this...  */
	pit->timer_cntl = 0xe0;
	pit->gen_cntl = 0x00;
	pit->serv_req = 0x08;
	pit->a_cntl = 0xc0;
	pit->a_dir = 0x00;
	pit->b_cntl = 0xf8;
	pit->b_dir = 0x00;
	pit->c_data = 0xc0;
	pit->c_dir = 0xd0;

	strncpy (boot_info.command_line, bootstr,
			    sizeof (boot_info.command_line));

	boot_info.machtype = MACH_BESTA_CP31;
	boot_info.cputype = __set_cpu_fpu_type();

	/*  memory initialize stuff   */
	memory = 0xfb000000;    /*  all 32bit possible memory area   */

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

	/*  this should be overloaded later by memory type testing stuff...  */
	boot_info.memory[0].addr = 0;
	boot_info.memory[0].size = memory;     /*  size in bytes   */
	boot_info.num_memory = 1;       /*  one  block   */

	/*  stram size  checking...   */
	memory = __stram_check (0xff040000, 0xff080000, 0x1000);
	cp31_info->stram_size = memory - 0xff040000;

	/*  At this point check for any other present memory...  */
	__mem_collect (MAX_SCAN_ADDR, 512 * 1024);

#if 0
	{ struct besta_memory_info *ptr;
	  int num;
	  for (num = 0, ptr = besta_memory_info; ptr; num++, ptr = ptr->next) ;

	  besta_memory_info[num - 1].next = &besta_memory_info[num];
	  besta_memory_info[num].addr = 0xff040000;
	  besta_memory_info[num].size = cp31_info->stram_size;
	  besta_memory_info[num].next = NULL;
	}
#endif

	return;
}

void besta_cache_init (void) {
	int cacr;

	cacr = 0x0101;  /*  the common default good value   */

	cacr |= 0x0808;
	__asm volatile ("mov.l  %0,%%cacr" : : "r" (cacr));

	return;
}


/*  real time clock stuff   */

struct cp31_rtc {
	unsigned char sec;
	unsigned char sec10;
	unsigned char min;
	unsigned char min10;
	unsigned char hour;
	unsigned char hour10;
	unsigned char day;
	unsigned char day10;
	unsigned char mon;
	unsigned char mon10;
	unsigned char year;
	unsigned char year10;
	unsigned char week;
	unsigned char cntl0;
	unsigned char cntl1;
	unsigned char cntl2;
};

static int rtc_hwclk (int rw, struct hwclk_time *);
static int rtc_set_clock_mmss (unsigned long time);

static struct clock_ops rtc_clock_ops = {
	rtc_hwclk,
	rtc_set_clock_mmss,
	NULL
};

int cp31_rtcinit (void) {
	volatile struct cp31_rtc *rtc = (struct cp31_rtc *) RTC_ADDR;
	int value;

	if (VME_probe (&rtc->cntl0, 0, PROBE_READ, PORT_BYTE) != 0 ||
	    VME_probe (&rtc->cntl2, &value, PROBE_READ, PORT_BYTE) != 0 ||
	    (value & 0x0f) == 0x0f
	)  return 0;    /*  present test failed   */

	register_clock (&rtc_clock_ops);

	return 1;
}


static int rtc_hwclk (int rw, struct hwclk_time *time) {
	volatile struct cp31_rtc *rtc = (struct cp31_rtc *) RTC_ADDR;
	int i, j;

	if (!rw) {      /*  read   */

	    rtc->cntl0 |= 0x01;     /*  hold   */

	    for (i = 0; i < 100 && (rtc->cntl0 & 0x02); i++) {
		rtc->cntl0 &= ~0x01;
		for (j = 0; j < 1000; j++) ;
		rtc->cntl0 |= 0x01;
	    }
	    if (i >= 100)  return -EIO;

	    time->sec = (rtc->sec10 & 0x07) * 10 + (rtc->sec & 0x0f);
	    time->min = (rtc->min10 & 0x07) * 10 + (rtc->min & 0x0f);
	    time->hour = (rtc->hour10 & 0x03) * 10 + (rtc->hour & 0x0f);
	    time->day = (rtc->day10 & 0x3) * 10 + (rtc->day & 0x0f);
	    time->mon = (rtc->mon10 & 0x1) * 10 + (rtc->mon & 0x0f) - 1;
	    time->year = (rtc->year10 & 0xf) * 10 + (rtc->year & 0xf);
	    time->wday = -1;    /*  ignore it, because now I havn`t info...  */

	    if (time->year < 70)  time->year += 100;

	    rtc->cntl0 &= ~0x01;

	} else {        /*  write   */

	    rtc->cntl2 = 0x07;
	    rtc->cntl2 = 0x06;
	    rtc->cntl0 |= 0x01;     /*  hold   */

	    for (i = 0; i < 100 && (rtc->cntl0 & 0x02); i++) {
		rtc->cntl0 &= ~0x01;
		for (j = 0; j < 1000; j++) ;
		rtc->cntl0 |= 0x01;
	    }
	    if (i >= 100)  return -EIO;

	    time->mon += 1;     /*  0 - 11  -->  1 - 12   */

	    rtc->sec = (time->sec % 10) & 0x0f;
	    rtc->sec10 = (time->sec / 10) & 0x07;
	    rtc->min = (time->min % 10) & 0x0f;
	    rtc->min10 = (time->min / 10) & 0x07;
	    rtc->hour = (time->hour % 10) & 0x0f;
	    rtc->hour10 = (time->hour / 10) & 0x03;
	    rtc->day = (time->day % 10) & 0x0f;
	    rtc->day10 = (time->day / 10) & 0x03;
	    rtc->mon = (time->mon % 10) & 0x0f;
	    rtc->mon10 = (time->mon / 10) & 0x01;
	    rtc->year = (time->year % 10) & 0x0f;
	    rtc->year10 = (time->year / 10) & 0x0f;

	    rtc->cntl0 &= ~0x01;
	    rtc->cntl2 = 0x04;
	}

	return 0;
}

/*  most common device-independed way is to use hwclk private method...  */
static int rtc_set_clock_mmss (unsigned long newtime) {
	struct hwclk_time time;
	int err;

	err = rtc_hwclk (0, &time);     /*  read curr hw time   */
	if (err)  return err;

	time.sec = newtime % 60;
	time.min = (newtime / 60) % 60;

	return  rtc_hwclk (1, &time);   /*  set changed time   */
}


#if 0
int print_long (unsigned long value) {
	int i, j;
	char c[9];

	c[8] = '\0';
	for (i = 7; i >= 0; i--) {
	    j = value & 0xf;
	    value >>= 4;

	    if (j >= 10)  c[i] = j + ('a' - 10);
	    else  c[i] = j + '0';
	}

	sio_putstring (c);

	return 0;
}
#endif
