/*
 * besta/hcpu30.c -- Main source file for HCPU30 configuration.
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
#include "hcpu30.h"


/*      Machine-depended routines.       */

/*    `To make Linux happy' routines.  */

static int hcpu30_make_Linux_happy_ENXIO (void) {  return -ENXIO; }
static void hcpu30_make_Linux_happy_void (void) {}
static void *hcpu30_make_Linux_happy_NULL (void) {  return NULL; }
static int hcpu30_make_Linux_happy_0 (void) {  return 0; }

static unsigned long hcpu30_gettimeoffset (void) {
	return 0;   /*  don`t know how to read hardware timer   */
}

static void hcpu30_process_int (int vec, struct pt_regs *fp) {
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

extern void hcpu30_sched_init (intfunc handler);
extern void hcpu30_gettod (int *, int *, int *, int *, int *, int *);
extern int hcpu30_set_clock_mmss (unsigned long);
extern int hcpu30_hwclk (int, struct hwclk_time *);
extern void hcpu30_reset (void);

/*  hcpu30 should not use irq (non-vec) interrupts  */
static void hcpu30_irq_handler (int irq, void *dev_id, struct pt_regs *fp) {

	hcpu30_process_int (irq + VEC_SPUR, fp);

	return;
}

static void (*hcpu30_irq_handlers[SYS_IRQS])(int, void *, struct pt_regs *) = {
	hcpu30_irq_handler, hcpu30_irq_handler, hcpu30_irq_handler,
	hcpu30_irq_handler, hcpu30_irq_handler, hcpu30_irq_handler,
	hcpu30_irq_handler, hcpu30_irq_handler
};

int hcpu30_debug_mode = 0;
static void hcpu30_debug_init (void) {

	hcpu30_debug_mode = 1;

	return;
}


void config_besta (void) {

	mach_sched_init = hcpu30_sched_init;
	mach_gettod = hcpu30_gettod;
	mach_set_clock_mmss = hcpu30_set_clock_mmss;
	mach_gettimeoffset = hcpu30_gettimeoffset;
	mach_hwclk = hcpu30_hwclk;
	mach_process_int = hcpu30_process_int;
	mach_reset = hcpu30_reset;
	mach_default_handler = &hcpu30_irq_handlers;
	mach_debug_init = hcpu30_debug_init;

	/*  hcpu30 dma processor can access all DRAM and VME memory   */
	mach_max_dma_address = 0xfcffffff;

/*  This pointers are not NULL-checked anyvere.   */
	mach_init_IRQ = (void *) hcpu30_make_Linux_happy_void;
	mach_mksound = (void *) hcpu30_make_Linux_happy_void;
	mach_video_setup = (void *) hcpu30_make_Linux_happy_void;
	mach_keyb_init = (void *) hcpu30_make_Linux_happy_ENXIO;
	mach_request_irq = (void *) hcpu30_make_Linux_happy_ENXIO;
	mach_free_irq = (void *) hcpu30_make_Linux_happy_ENXIO;
	mach_fb_init = (void *) hcpu30_make_Linux_happy_NULL;
	mach_get_irq_list = (void *) hcpu30_make_Linux_happy_0;
}


/*      Console init specific stuff       */

extern void register_console (void (*)(const char *));
extern void xdus_putstring (const char *);

unsigned long besta_console_init (unsigned long mem_start,
					    unsigned long mem_end) {
	register_console (xdus_putstring);

	return mem_start;
}


/*      Base devices init routine (i.e., motherboard etc.)      */

extern int hcpu30_xdusinit (struct async_struct *, int);
extern void xdsk_init (void);
extern void xfd_init (void);
extern void xcen_init (void);
extern int xlan_init (void);

extern struct linux_binfmt svr3_format;

static void hcpu30_notify_board (void);


void besta_dev_init (void) {
	struct hcpu30_info *hcpu30_info = (struct hcpu30_info *)
						&boot_info.bi_un.bi_hcpu30;

	/*  initialize xdus two ports   */
	hcpu30_xdusinit(rs_table, 0);

	/*  initialize SCSI devices   */
	xdsk_init ();
	/*  initialize floppy drivers   */
	xfd_init ();
	/*  initialize centronics port  */
	xcen_init ();
	/*  initialize ethernet   */
	if (xlan_init ())  hcpu30_info->flags |= XLAN_PRESENT;
	else  hcpu30_info->flags &= ~XLAN_PRESENT;

	printk ("SVR3.1/m68k binary compatibility "
		"code copyright 1996 Dm.K.Butskoy\n");
	register_binfmt (&svr3_format);

	hcpu30_notify_board();

	/*   Extra devices initializations (VME, peripherals, etc.).  */

	besta_drivers_init();

	VME_init();

	return;
}

static void hcpu30_notify_board (void) {
	int basic_memory_size = boot_info.memory[0].size >> 20;
	struct hcpu30_info *hcpu30_info = (struct hcpu30_info *)
						&boot_info.bi_un.bi_hcpu30;
	int dip_switch = hcpu30_info->dip_switch & 0xff;
	int xlan_present = hcpu30_info->flags & XLAN_PRESENT;
	int cacr;

	printk ("    BESTA  basic hardware found:   HCPU30%s/%d    board\n",
			(xlan_present ? "" : "-L"), basic_memory_size);

	printk ("(mc68030/mc68882, %dM RAM, SCSI, 2 serials, "
		"floppy, centronix, %sclock,\n",
		basic_memory_size, (xlan_present ? "LANCE, " : ""));

	__asm volatile ("mov.l %%cacr, %0" : "=da" (cacr) : );

	printk ("VME: master %s %s, data burst %s, "
		"cacr=0x%04x, dip switch=0x%02x, %ldk stram)\n",
		(dip_switch & 0x20) ? "REC" : "ROR",
		(dip_switch & 0x10) ? "16bit" : "32bit",
		(cacr & 0x1000) ? "on" : "off", cacr, dip_switch,
		hcpu30_info->stram_size >> 10);
	return;
}


void besta_get_model (char *model) {
	struct hcpu30_info *hcpu30_info = (struct hcpu30_info *)
						&boot_info.bi_un.bi_hcpu30;

	sprintf (model, "HCPU30%s/%lu  VME board",
			((hcpu30_info->flags & XLAN_PRESENT) ? "" : "-L"),
			(boot_info.memory[0].size >> 20));

	return;
}

int besta_get_hardware_list (char *buffer) {

	/*  currently nothing   */

	return 0;
}
