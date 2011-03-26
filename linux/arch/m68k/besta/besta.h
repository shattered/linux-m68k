/*
 * besta/besta.h -- Common besta source header.
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

#define MACH_BESTA_BASE         1000
#define MACH_BESTA_HCPU30       (MACH_BESTA_BASE + 0)
#define MACH_BESTA_CP31         (MACH_BESTA_BASE + 1)

typedef void (*intfunc) (int, void *, struct pt_regs *);

extern intfunc besta_handlers[];
extern void *besta_intr_data[];

#define MAX_SERIAL      64
#define SERIAL_START    64
extern int VME_serial_cnt;

struct VME_board {
	const char *name;
	int     addr;
	int     vect;
	int     lev;
	int     major;
	void  (*init) (struct VME_board *, int);
	int     present;
	struct VME_board *next;
};

extern struct VME_board *VME_base;

#define PROBE_READ      0
#define PROBE_WRITE     1
#define PORT_BYTE       1
#define PORT_WORD       2
#define PORT_LONG       4
#define DIR_FORWARD     0
#define DIR_BACKWARD    1
extern int VME_probe (volatile void *addr, int *value, int rw, int op_size);
extern int __rw_while_OK (unsigned start_area, unsigned end_area, int rw,
			    int value_to_write, int op_size, int direction);
extern int __set_cpu_fpu_type (void);

extern void VME_init (void);
extern void VME_deinit (void);
extern void besta_drivers_init (void);
extern int VME_set_ios (unsigned long, unsigned long, unsigned long);

#define clear_data_cache(A,B)   \
    do {                        \
	register int i;                         \
	__asm volatile ("mov.l %%cacr,%0\n\t"   \
			"or.w &0x0800,%0\n\t"   \
			"mov.l %0,%%cacr"       \
			: "=d" (i)              \
			:                       \
	);                                      \
    } while (0);

extern const char *besta_get_serial_type (int type);
extern int besta_add_serial_type (const char *name, int type);

extern int besta_get_vect_lev (char *name, int *vector, int *level);
extern int get_unused_vector (void);

struct hwclk_time;

struct clock_ops {
	int (*hwclk) (int rw, struct hwclk_time *time);
	int (*set_clock_mmss) (unsigned long time);
	struct clock_ops *next;
};

extern struct clock_ops *besta_clock_base;

extern int register_clock (struct clock_ops *);

extern int besta_cacr_user;


/*  Memory collect/testing stuff...  */
extern unsigned int __stram_check (unsigned start_area, unsigned end_area,
							unsigned addr_incr);
extern void __mem_collect (unsigned int max_addr, unsigned int addr_incr);
extern int __mem_accs_per_clock (int, int, int, int);
extern int __mem_accs_per_cycle (void);
extern int __try_burst_ok (int);

#define MAX_SCAN_ADDR   0x80000000

struct besta_memory_info {
	unsigned int   addr;
	unsigned int   size;
	unsigned short time;    /*  in nanoseconds   */
	unsigned char  port;    /*  1, 2 or 4 bytes bus   */
	unsigned char  flags;   /*  burst hundled, fast/slow, etc.  */
	struct besta_memory_info *next;
};
#define CAN_BURST       0x01
#define IN_STRD         0x02

#define MIN_VME_TIME    300     /*  ???, in nanoseconds  */

extern struct besta_memory_info besta_memory_info[];

