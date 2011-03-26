/*
 * besta/VME.c - Besta`s generic and VME-specific routines.
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
#include <linux/malloc.h>
#include <linux/swap.h>
#include <linux/sched.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/serial.h>
#include <linux/linkage.h>
#ifdef CONFIG_BLK_DEV_RAM
#include <linux/blk.h>
#endif

#include <asm/system.h>
#include <asm/pgtable.h>
#include <asm/traps.h>
#include <asm/irq.h>

#include "besta.h"

unsigned long highest_memory = MAX_SCAN_ADDR;

intfunc besta_handlers[256] = { 0, };
void *besta_intr_data[256] = { 0, };


int VME_serial_cnt = 8;     /*  Really only 2 used by xdus on hcpu30.  */

#define HCEN_MAJOR      49
#define IOS_MAJOR       50
#define MD10_MAJOR      51
#define TLM_MAJOR       52
#define SOCKM_MAJOR     54
#define CLONE_MAJOR     55
#define INET_STREAM_MAJOR       56
#define INET_DGRAM_MAJOR        57
#define UNIX_STREAM_MAJOR       58
#define UNIX_DGRAM_MAJOR        59
#define VME_MAJOR       60


/*  Initialization stuff for hardware independent drivers.  */

#define base(X...)
#define VME(X...)
#define driver(NAME,MAJOR,INIT_FUNC) \
extern void INIT_FUNC (const char *, int);

#include "besta.conf"

#undef base
#undef VME
#undef driver

static struct besta_drivers_struct {
	const char *name;
	int major;
	void (*init) (const char *, int);
} besta_drivers[] = {

#define base(X...)
#define VME(X...)
#define driver(NAME,MAJOR,INIT_FUNC) \
	{ NAME, MAJOR, INIT_FUNC },

#include "besta.conf"

#undef base
#undef VME
#undef driver

	{ NULL, 0, NULL }       /*  To make compiler happy   */
};

#define NUM_BESTA_DRIVERS  \
     ((sizeof (besta_drivers) / sizeof (*besta_drivers)) - 1)

void besta_drivers_init (void) {
	int i;

	for (i = 0; i < NUM_BESTA_DRIVERS; i++)
		besta_drivers[i].init (besta_drivers[i].name,
				       besta_drivers[i].major);
	/*  no idea what also   */
	return;
}


/*  Initialization stuff for VME bus`s hardware drivers.  */

#define base(X...)
#define driver(X...)
#define VME(NAME,ADDR,VECTOR,LEVEL,MAJOR,INIT_FUNC) \
extern void INIT_FUNC (struct VME_board *, int);

#include "besta.conf"

#undef base
#undef driver
#undef VME

static struct VME_board VME_boards[] = {

#define base(X...)
#define driver(X...)
#define VME(NAME,ADDR,VECTOR,LEVEL,MAJOR,INIT_FUNC) \
	{ NAME, ADDR, VECTOR, LEVEL, MAJOR, INIT_FUNC, 0, NULL },

#include "besta.conf"

#undef base
#undef driver
#undef VME

	{ NULL, 0, 0, 0, 0, NULL, 0, NULL }     /*  To make compiler happy   */
};

#define VME_NUM_BOARDS  ((sizeof (VME_boards) / sizeof (*VME_boards)) - 1)

/*  special object to handle on-VME and static ramdisk memory...  */
static void VME_mem_init (struct VME_board *VME, int on_off);
static struct VME_board VME_memory = {
	"VME_mem",      /*  useful, if it is a static ram disk (on slow mem) */
	0,
	0,
	0,
	VME_MAJOR,
	VME_mem_init,
	0,
	NULL
};

struct VME_board *VME_base = NULL;

#define VME_NAME(A)     (A ? A : "unknown")

void VME_init (void) {
	int i;
	struct VME_board *VME;
	struct besta_memory_info *ptr;

	/*  validate the chain   */
	for (i = 0; i < VME_NUM_BOARDS; i++) {
		VME_boards[i].next = &VME_boards[i+1];
		VME_boards[i].present = 0;      /*  setted by init() method */
	}

	/*  If some of present memory chunk is an on-VME-memory,
	   include it into the VME boards chain.
	*/
	for (ptr = besta_memory_info; ptr; ptr = ptr->next)
		if (ptr->time >= MIN_VME_TIME)  break;
	if (ptr) {      /*  i.e., present   */
	    VME_base = &VME_memory;
	    VME_base->next = VME_NUM_BOARDS > 0 ? VME_boards : NULL;
	}
	else if (VME_NUM_BOARDS == 0)  return;  /*  nothing to do   */
	else
	    VME_base = VME_boards;  /*  pointer to the start of chain  */


	/*  Initialization stuff.  */
	printk ("Probing VME boards:\n");

	for (VME = VME_base; VME; VME = VME->next) {

	    if (VME->addr > 0 && VME->addr < high_memory) {
		printk ("   for %s: address 0x%08x inside ordinary memory\n",
			    VME_NAME (VME->name), VME->addr);
		continue;
	    }
	    if (VME->addr &&
		    VME->addr < 0xfc000000 &&
			VME->addr >= MAX_SCAN_ADDR
	    ) {
		printk ("    for %s at 0x%08x: only `0xfc000000 - 0xffffffff'"
			" i/o area currently supported\n",
			VME_NAME (VME->name), VME->addr);
		continue;
	    }
	    if (VME->lev > 7 || VME->lev < 0) {
		printk ("    for %s at 0x%08x: bad interrupt level value %x"
			" (should be [1 - 7] or 0)\n",
			VME_NAME (VME->name), VME->addr, VME->lev);
		continue;
	    }
	    if (VME->lev) {
		if (VME->vect < 0) {
		    /*  The board driver should get_unused_vector()...  */
		}
		else if (VME->vect == 0) {
		    VME->vect = VEC_SPUR + VME->lev;
		    /*  The above is not enough.
			Here should be stuff for autovectors.
			should...
		    */
		}
		else if (VME->vect < 64 || VME->vect > 255) {
		    printk ("    for %s at 0x%08x: bad vector value 0x%x "
			    "(should be inside [0x40 - 0x100)\n",
			    VME_NAME (VME->name), VME->addr, VME->vect);
		    continue;
		}
		/*  The board driver should check case
		   this interrupt vector is already in use...
		*/
	    }

	    /*  OK   */

	    if (VME->init)  VME->init (VME, 0);   /*  init...  */

	}

	printk ("done\n");

	return;
}

/*  This routine should be called at reboot time to set some VME boards
   to initial state, because it can not be done by boot loader...
*/
void VME_deinit (void) {
	struct VME_board *VME;

	for (VME = VME_base; VME; VME = VME->next)
		if (VME->present && VME->init)  VME->init (VME, 1);

	return;
}



/*  Set_ios routine. Needable by many drivers (ioses, graphics, etc.).

    This kernel routine has the same effect as users code:
	fd = open ("/dev/mem", perm);
	mmap (virt_addr, length,
		PROT_READ | PROT_WRITE, MAP_FIXED | MAP_SHARED,
		  fd, phys_addr);
	close (fd);

    but skipping checks and real opening.
    Also, it add `no-cachable' attribute to mmapped pages, because
    most often case is  this is device ports` access memory.
*/

int VME_set_ios (unsigned long vaddr, unsigned long paddr,
						unsigned long size) {
	struct vm_area_struct *vma;

	/*  Checking.   */
	if (size == 0)  return 0;   /* nothing  */

	if ((vaddr ^ paddr) & ~PAGE_MASK)  return -EINVAL;

	size += (vaddr - (vaddr & PAGE_MASK));

	vaddr &= PAGE_MASK;
	paddr &= PAGE_MASK;
	size = (size + ~PAGE_MASK) & PAGE_MASK;

	/*  mmapping   */

	/*  It is better to protect mmapping to ordinary memory,
	   to avoid some hackers (as I am).     */
	if (!suser() && (paddr < high_memory ||
			 (paddr + size) < high_memory)
	)  return -EPERM;

	vma = (struct vm_area_struct *)
		kmalloc (sizeof (struct vm_area_struct), GFP_KERNEL);
	if (!vma)  return -ENOMEM;

	vma->vm_mm = current->mm;
	vma->vm_start = vaddr;
	vma->vm_end = vaddr + size;
	vma->vm_flags = VM_READ | VM_WRITE |
			VM_MAYREAD | VM_MAYWRITE |
			VM_SHARED | VM_MAYSHARE;
	vma->vm_page_prot = __pgprot(_PAGE_PRESENT | _PAGE_ACCESSED |
						     _PAGE_NOCACHE030);

	vma->vm_ops = NULL;
	vma->vm_offset = paddr;
	vma->vm_inode = NULL;
	vma->vm_pte = 0;

	/*  NOTE: `do_munmap()' works only inside [0, TASK_SIZE) area.
	   If  vaddr > TASK_SIZE (defined in include/asm/processor.h),
	   `remap_page_range()' and other works, but `do_munmap()' confused.
	   Currently we ignore this confuse, hoping we never remapp such
	   areas per process life.
	*/
	do_munmap(vaddr, size);   /* Clear old maps */

	if (remap_page_range (vma->vm_start, vma->vm_offset,
			      vma->vm_end - vma->vm_start, vma->vm_page_prot)
	) { kfree (vma); return -EAGAIN; }

	insert_vm_struct (current->mm, vma);
	merge_segments (current->mm, vma->vm_start, vma->vm_end);
	current->mm->total_vm += size >> PAGE_SHIFT;

	return 0;
}


/*  Common besta stuff...  */

#ifdef CONFIG_PROC_FS
static struct besta_serial_type {
	const char *name;
	int type;
	struct besta_serial_type *next;
} besta_serial_types_base = {
	"unknown",
	0,
	NULL
};

const char *besta_get_serial_type (int type) {
	struct besta_serial_type *bstp;

	for (bstp = &besta_serial_types_base; bstp; bstp = bstp->next)
		if (bstp->type == type)  break;

	return bstp ? bstp->name : "unknown";
}

/*  if type == 0, then generate free type number, else use received type.
    returns real type setting to for this name.
*/
int besta_add_serial_type (const char *name, int type) {
	struct besta_serial_type *bstp, *bstp_last;
	int maxtype = 0;

	bstp = bstp_last = &besta_serial_types_base;

	/*  Is this typename or this typenumber in use... */
	for ( ; bstp; bstp = bstp->next) {
	    if (!strcmp (bstp->name, name)) {
		if (type && bstp->type != type)  return -EBUSY;
		else  return bstp->type;
	    } else
		if (type && bstp->type == type)  return -EBUSY;

	    /*  also compute the unused type number...   */
	    if (bstp->type > maxtype)  maxtype = bstp->type;
	    bstp_last = bstp;
	}

	/*  OK, new typename   */
	if (!type)  type = maxtype + 1;

	bstp = (struct besta_serial_type *)
			    kmalloc (sizeof (*bstp), GFP_KERNEL);
	if (!bstp)  return -ENOMEM;

	bstp->name = name;
	bstp->type = type;
	bstp->next = NULL;
	bstp_last->next = bstp;

	return type;
}
#endif  /*  CONFIG_PROC_FS   */


/*  besta_get_vect_lev()  gets vector and level values
   for base board drivers by its names. If such data not present,
   returns 0 (will be used default algorithm), else returns 1 .
    Initialization comes from `besta.conf' macro-collection file.
*/

static struct base_board_tune {
	const char *name;
	int      vector;
	unsigned level;
} base_board_tunes[] = {

#define base(NAME,VECTOR,LEVEL) \
	{ NAME, VECTOR, LEVEL },
#define driver(X...)
#define VME(X...)

#include "besta.conf"

#undef base
#undef driver
#undef VME

	{ NULL, 0, 0 }      /*  To make compiler happy   */
};

#define NUM_BASE_TUNES  \
	((sizeof (base_board_tunes) / sizeof (*base_board_tunes)) - 1)

int besta_get_vect_lev (char *name, int *vecptr, int *levptr) {
	int i;
	int vector;
	unsigned int level;

	if (!vecptr || !levptr)  return 0;

	/*  find a device by name...  */
	for (i = 0; i < NUM_BASE_TUNES; i++)
		if (!strcmp (base_board_tunes[i].name, name))  break;

	if (i == NUM_BASE_TUNES)  return 0;

	vector = base_board_tunes[i].vector;
	level = base_board_tunes[i].level;

	if (vector > 255 || (vector >= 0 && vector < 64) || level > 7) {
	    printk ("%s: bad vector %d and/or level %d values, "
		    "use default instead\n",
		    base_board_tunes[i].name, vector, level);
	    return 0;
	}

	if (vector < 0)  vector = get_unused_vector();

	if (besta_handlers[vector] != NULL) {
	    printk ("%s: vector %d already used, use default instead\n",
					    base_board_tunes[i].name, vector);
	    return 0;
	}

	*vecptr = vector;
	*levptr = level;

	return 1;
}


int get_unused_vector (void) {
	static int first_free = 64 + 4;     /*  64 + paranoia   */

	while (besta_handlers[first_free] != NULL &&
	       first_free < 256
	)  first_free++;

	if (first_free >= 256) {
	    /*  256 - 64 = 192  devices eat the one processor.
	       What should we do ???
	    */
	    printk ("Too many vectors used! Last #255 overloaded!\n");

	    return 255;
	}

	return first_free++;
}


/*   Real time clock stuff. There may be very many clocks on VME bus...  */

struct clock_ops *besta_clock_base = NULL;

int register_clock (struct clock_ops *new) {
	struct clock_ops *tmp;

	if (!new || !new->hwclk || !new->set_clock_mmss)  return -EINVAL;

	new->next = NULL;

	if (!besta_clock_base)  besta_clock_base = new;
	else {
	    for (tmp = besta_clock_base; tmp->next; tmp = tmp->next) ;
	    tmp->next = new;
	}

	return 0;
}

/*  don`t want to unregister while...  */


/*  To full `besta_cacr_user' value by boot string... */
int besta_cacr_user = -1;

void besta_cacr_setup (char *str, int *ints) {

	if (ints[0] <= 0)  return;

	besta_cacr_user = ints[1];

	return;
}



/*  Memory stuff specific routines.  */

#define BESTA_NUM_MEMORY        8
struct besta_memory_info besta_memory_info[BESTA_NUM_MEMORY] = {{0,}, };

/*  we assume memory size less than 4Mb is not enough...  */
#define LOW_MEM_LIMIT   (4 * 1024 * 1024)
/*  5% of hole memory can be used for empty area `mem_map's   */
#define BAD_MEM_MAP_PERC        20

/*  When invokes, boot_info.memory[0] should be valid,
   as basic memory chunk info.
*/
void __mem_collect (unsigned int max_addr, unsigned int addr_incr) {
	unsigned int addr, end_addr;
	int num = 1;

	besta_memory_info[0].addr = boot_info.memory[0].addr;
	besta_memory_info[0].size = boot_info.memory[0].size;

	/*  Check all areas higher than currently high_memory ...  */
	addr = boot_info.memory[0].addr + boot_info.memory[0].size;

	/*  It is already determined that there is no memory on `addr'  */
	addr += addr_incr;

	for ( ; addr < max_addr; addr += addr_incr) {
	    unsigned int value = 0;

	    /*  first probe to read, hcpu30 crashes on writes...   */
	    if (VME_probe ((void *) addr, 0, PROBE_READ, PORT_LONG))
		    continue;
	    if (VME_probe ((void *) addr, &value, PROBE_WRITE, PORT_LONG))
		    continue;
	    if (VME_probe ((void *) addr, &value, PROBE_READ, PORT_LONG) ||
		value != 0
	    )  continue;

	    end_addr = __rw_while_OK (addr, max_addr, PROBE_WRITE, 0,
						    PORT_LONG, DIR_FORWARD);

	    besta_memory_info[num].addr = addr;
	    besta_memory_info[num].size = end_addr - addr;
	    besta_memory_info[num - 1].next = &besta_memory_info[num];
	    num++;

	    if (num >= BESTA_NUM_MEMORY)  break;

	    /*  backward aligned addr, to get the next addr correctly...  */
	    addr = end_addr & ~(addr_incr - 1);
	}

	if (/*  there is no enough memory for good work...  */
	    boot_info.memory[0].size < LOW_MEM_LIMIT &&
	    /*  there are more memory chunks...  */
	    num > 1 &&
	    /*  empty area between the chunks are not very big...  */
	    (besta_memory_info[1].addr - besta_memory_info[0].size) <=
		(besta_memory_info[0].size + besta_memory_info[1].size) *
		    (PAGE_SIZE / (BAD_MEM_MAP_PERC * sizeof (mem_map_t)))
	)  boot_info.memory[0].size =
		besta_memory_info[1].addr + besta_memory_info[1].size;
		/*  empty area, etc., will be handled later (mem_init())  */

	return;
}

/*  It may be an empty areas, allowing as to read/write a gabage,
   or overloaded areas to the same stram memory, etc.

    Note:  we use `(end - addr) > 0' instead of `addr < end', because
   `end' might have a value of overflowed null `0x00000000'...
*/
unsigned int __stram_check (unsigned int start_addr, unsigned int end_addr,
							unsigned int incr) {
	unsigned char *start, *end, *addr;
	unsigned int old, new;

	end_addr = __rw_while_OK (start_addr, end_addr, PROBE_READ, 0,
						    PORT_BYTE, DIR_FORWARD);
	start = (unsigned char *) start_addr;
	end = (unsigned char *) end_addr;

	/*  write + store test...  */
	for (addr = start; (end - addr) > 0; addr += incr) {

	    old = *addr;
	    new = ~old;

	    if (VME_probe (addr, &new, PROBE_WRITE, PORT_BYTE))
		    break;

	    if (*addr != (unsigned char) new)  break;
	    *addr = old;
	}
	if ((end - addr) > 0)  end = addr;

	if (end == start)  return (unsigned int) end;

	/*  check for overloaded areas...  */
	for (addr = start; (end - addr) > 0; addr += incr)  *addr = 0;
	for (addr = start; (end - addr) > 0; addr += incr) {
	    unsigned char *p;

	    *addr = 1;
	    for (p = addr + incr; (end - p) > 0; p += incr)
		    if (*p != 0)  break;
	    *addr = 0;

	    if ((end - p) > 0)  end = p;
	}

	return  (unsigned int) end;
}


#define ADDR_INCR       (512 * 1024)    /*  step for checking   */
#define CRITERIA        5       /*  100%/CRITERIA may be differ   */

__inline static int hear (int a, int b) {
	int m = (a + b) / 2;
	int s = (a > b) ? (a - b) : (b - a);

	return  s < m / CRITERIA;
}

static void besta_mem_test (void) {
	struct besta_memory_info *ptr;
	unsigned short flags;
	register unsigned long cacr, only_insn;
	unsigned int ticks, scale;
	int num, tests;
	unsigned int tc, tc_null = 0;

	scale = (1000000000UL / HZ) / __mem_accs_per_cycle();

	for (num = 0, ptr = besta_memory_info;
		num < BESTA_NUM_MEMORY && ptr; num++, ptr = ptr->next) ;

	save_flags (flags);
	/*  ignore interrupts other than clock   */
	__asm volatile ("mov.w &0x2500,%%sr" : : );

	/*  !!! set mmu off (transparent physical...)   */
	__asm volatile ("pmove  %%tc,(%0)" : : "a" (&tc) : "memory" );
	__asm volatile ("pmove  (%0),%%tc" : : "a" (&tc_null));


	/*  synchronization   */
	ticks = jiffies;
	while (ticks == jiffies) ;

	/*  set off data cache...  */
	__asm volatile ("mov.l %%cacr,%0" : "=d" (cacr) : );
	only_insn = cacr & 0xff;    /*  only insn cache enabled...  */
	__asm volatile ("mov.l %0,%%cacr" : : "d" (only_insn));


	/*  Go throw all the present memory   */
	for (ptr = besta_memory_info; ptr; ptr = ptr->next) {
	    unsigned int start, end, addr_incr;
	    unsigned int res_onec, res_twoc, res_more;

	    start = ptr->addr;
	    end = ptr->addr + ptr->size;

	    /*  for `addr_incr' we use:
		  a     =  x  x  x  x  1  0  0
		 -a     = ~x ~x ~x ~x  1  0  0
	       a & (-a) =  0  0  0  0  1  0  0
				       ^  lost sign. bit to vary...
	    */
	    addr_incr = start | end;
	    addr_incr = addr_incr & (-addr_incr);
	    if (addr_incr > ADDR_INCR)  addr_incr = ADDR_INCR;

	    res_onec = res_twoc = res_more = 0;

	    for (tests = 0; (end - start) > 0; start += addr_incr, tests++) {
		/*  forward alignment + offset by page when start == 0   */
		unsigned int addr = (start + PAGE_SIZE) & PAGE_MASK;
		unsigned int new_onec, new_more;
		int d, port, time, flags;

		if (ptr->flags & CAN_BURST) {
		    /*  one bus cycle is guaranteed...  */
		    new_onec = __mem_accs_per_clock (addr+3, 0, 0, PORT_BYTE);

		    if (hear (new_onec, res_onec) && __try_burst_ok (addr))
			    continue;       /*  the same memory   */

		    res_onec = new_onec;
		    /*  4 cycles for 8bit bus, 3 for 16bit, 2 for 32bit bus  */
		    res_more = __mem_accs_per_clock (addr+3, 0, 0, PORT_LONG);

		} else {
		    /*  one bus cycle is guaranteed...  */
		    new_onec = __mem_accs_per_clock (addr+3, 0, 0, PORT_BYTE);
		    /*  4 cycles for 8bit bus, 3 for 16bit, 2 for 32bit bus  */
		    new_more = __mem_accs_per_clock (addr+3, 0, 0, PORT_LONG);

		    if (hear (new_onec, res_onec) &&
			hear (new_more, res_more)
		    )  continue;        /*  the same memory   */

		    res_onec = new_onec;
		    res_more = new_more;
		}

		/*  two bus cycles are guaranteed...  */
		res_twoc = __mem_accs_per_clock (addr + 3, 0, 0, PORT_WORD);

		if (res_onec <= res_twoc)  break;       /*  bad results   */

		/*  pure access time in nanoseconds   */
		time = scale / ((res_onec * res_twoc) / (res_onec - res_twoc));
		time += 5;
		time -= time % 10;      /*  10 ns rounding   */

		/*  `d' is a number of bus cycles...  */
		d = res_more * (res_onec - res_twoc);
		d = 1 + (res_twoc * (res_onec - res_more) + d / 2) / d;

		switch (d) {
		    case 2:  port = PORT_LONG;  break;
		    case 3:  port = PORT_WORD;  break;
		    case 4:  port = PORT_BYTE;  break;
		    default:  port = 0;  break;
		}

		flags = 0;
		if (port == PORT_LONG && __try_burst_ok (addr)) {
		    /*  There is no buserr on data burst. We should check
		      hear, is it real data burst (or breaked, etc.).
			We invalidate all the data cache,
		      try to full one 16byte`s line,
		      fix the data cache and check (by mem_accs_per_clock)
		      is access to 4-th long word an extern access or
		      access into data cache.
			If all the line is valid, (i.e., burst is ok)
		      we go to the cache (fast case), else go to the
		      external memory (slow case).
		    */
		    register unsigned int value0, value1;
		    int res;

		    value0 = only_insn | 0x1900;    /*  data flush + burst  */
		    value1 = only_insn | 0x0300;    /*  data freeze   */

		    __asm volatile ("mov.l %0,%%cacr\n\t"
				    "tst.l (%1)\n\t"
				    "mov.l %2,%%cacr"
				    :
				    : "d" (value0), "a" (addr), "d" (value1)
		    );

		    res = __mem_accs_per_clock (addr + 12, 0, 0, PORT_LONG);

		    __asm volatile ("mov.l %0,%%cacr" : : "d" (only_insn));

		    if (!hear (res, res_onec))  flags |= CAN_BURST;
		}

		if (!ptr->time) {
		    ptr->time = time;
		    ptr->port = port;
		    ptr->flags = flags;

		    continue;
		}

		/*  so, another memory type...  */

		ptr->size = start - ptr->addr;

		if (num >= BESTA_NUM_MEMORY)  break;

		besta_memory_info[num].addr = start;
		besta_memory_info[num].size = end - start;
		besta_memory_info[num].time = time;
		besta_memory_info[num].port = port;
		besta_memory_info[num].flags = flags;

		besta_memory_info[num].next = ptr->next;
		ptr->next = &besta_memory_info[num];
		num++;
		ptr = ptr->next;
	    }
	}


	/*  restore %cacr value   */
	__asm volatile ("mov.l %0,%%cacr" : : "d" (cacr));

	/*  !!!  restore mmu   */
	__asm volatile ("pmove (%0),%%tc" : : "a" (&tc));

	restore_flags (flags);

	return;
}


void VME_map_chunk (unsigned long addr, unsigned long size, int cacheable) {
	unsigned long *kpointerp;
	unsigned long physaddr;
	int pindex;     /* index into pointer table */
	pgd_t *page_dir;
	unsigned int page_flags;

	addr = PAGE_ALIGN (addr);
	size &= PAGE_MASK;

	if (boot_info.cputype & (CPU_68040 | CPU_68060))
		panic ("VME_map_chunk: currently only 680[23]0 supported.\n");

	page_flags = _PAGE_PRESENT | _PAGE_ACCESSED;
	if (!cacheable)  page_flags |= _PAGE_NOCACHE030;

	page_dir = pgd_offset_k (addr);
	if (!pgd_present (*page_dir)) {
	    /* we need a new pointer table */
	    kpointerp = (unsigned long *) get_kpointer_table ();

	    pgd_set (page_dir, (pmd_t *) kpointerp);
	    memset (kpointerp, 0, PTRS_PER_PMD * sizeof (pmd_t));

	} else
	    kpointerp = (unsigned long *) pgd_page (*page_dir);

	/*
	 * pindex is the offset into the pointer table for the
	 * descriptors for the current virtual address being mapped.
	 */
	pindex = (addr >> 18) & 0x7f;

	for (physaddr = addr; physaddr < addr + size; ) {

	    if (pindex > 127) {
		/* we need a new pointer table every 32M */

		kpointerp = (unsigned long *) get_kpointer_table ();
		pgd_set (pgd_offset_k (physaddr), (pmd_t *) kpointerp);
		pindex = 0;
	    }

	    /*
	     * 68030, use early termination page descriptors.
	     * Each one points to 64 pages (256K).
	     */

	    kpointerp[pindex++] = physaddr | page_flags;
	    physaddr += 64 * PAGE_SIZE;
	}

	return;
}


void besta_mem_init (unsigned long start_mem, unsigned long end_mem) {
	int codepages = 0, datapages = 0, emptypages = 0;
	unsigned long tmp;
	extern int _etext;
	struct besta_memory_info *ptr;
	int i;

	start_mem = PAGE_ALIGN(start_mem);
	end_mem &= PAGE_MASK;

	/*  full `besta_memory_info' chain...  */
	printk ("Memory testing... ");
	besta_mem_test();
	printk ("done\n");

	/*  check, if we raped any specific memory...  */
	for (i = 0; i < VME_NUM_BOARDS; i++)
	    if (VME_boards[i].addr && VME_boards[i].addr < end_mem)
		    end_mem = VME_boards[i].addr;
	high_memory = end_mem & PAGE_MASK;

	/*  handle mixed type memory...   */
	if (  /*  there is enough fast memory...  */
	    besta_memory_info[0].time < MIN_VME_TIME &&
	    besta_memory_info[0].size >= LOW_MEM_LIMIT &&
	      /*  ...and the next memory chunk is slow (VME)   */
	    (ptr = besta_memory_info[0].next) != NULL &&
	    ptr->time >= MIN_VME_TIME &&
	      /*  high_memory grows out fast chunk...  */
	    high_memory > besta_memory_info[0].size
	)  high_memory = besta_memory_info[0].size & PAGE_MASK;

	/*  release the basic memory   */
	for (ptr = besta_memory_info;
		     ptr && start_mem < high_memory; ptr = ptr->next
	) {
	    int high_chunk = ptr->addr + ptr->size;

	    if (start_mem < ptr->addr) {
		/*  i.e., there is empty area   */
		if (high_memory > ptr->addr)  start_mem = ptr->addr;
		else  {
		    high_memory = start_mem;
		    break;
		}
	    }

	    if (high_chunk > high_memory)  high_chunk = high_memory;

	    while (start_mem < high_chunk) {
		clear_bit(PG_reserved, &mem_map[MAP_NR(start_mem)].flags);
		start_mem += PAGE_SIZE;
	    }
	}


	/*  free memory + set DMA bit for HCPU30   */
	for (tmp = 0 ; tmp < end_mem ; tmp += PAGE_SIZE) {
	    if (boot_info.machtype != MACH_BESTA_HCPU30)
		    clear_bit (PG_DMA, &mem_map[MAP_NR(tmp)].flags);
	    if (PageReserved (mem_map+MAP_NR(tmp))) {
		if (tmp < (unsigned long) &_etext)  codepages++;
		else if (tmp < besta_memory_info[0].size)  datapages++;
		else  emptypages++;

		continue;
	    }

	    mem_map[MAP_NR(tmp)].count = 1;
#ifdef CONFIG_BLK_DEV_INITRD
	    if (!initrd_start ||
		(tmp < (initrd_start & PAGE_MASK) || tmp >= initrd_end))
#endif
		    free_page(tmp);
	}

	/*  mmap all unused memory (to make VME_chain work easy, if any)   */
	for (ptr = besta_memory_info; ptr; ptr = ptr->next) {
	    unsigned int end = ptr->addr + ptr->size;
	    unsigned int start = ptr->addr < high_memory
					? high_memory : ptr->addr;

	    if (start < end && end <= MAX_SCAN_ADDR)
				    VME_map_chunk (start, end - start, 0);
	}

	/*  count the `highest_memory' value, to correct virtual addresses
	  for vmalloc()
	*/
	highest_memory = high_memory;
	for (ptr = besta_memory_info; ptr; ptr = ptr->next) {
	    unsigned int end = ptr->addr + ptr->size;

	    if (end > high_memory && end <= MAX_SCAN_ADDR)
		    highest_memory = end;
	}

	printk ("Memory:\n");
	for (ptr = besta_memory_info; ptr; ptr = ptr->next) {
	    printk ("    0x%08x - 0x%08x :", ptr->addr, ptr->addr + ptr->size);

	    if (ptr->size % (1024 * 1024))  printk (" %dk", ptr->size >> 10);
	    else  printk (" %dMb", ptr->size >> 20);

	    printk (", %d bit, %d ns%s%s\n", ptr->port * 8, ptr->time,
		    (ptr->flags & CAN_BURST) ? ", burst ok" : "",
		    (ptr->addr < high_memory) ? ", used" : "");
	}

	printk("%uk/%luk available (%dk kernel code, %dk data",
	       nr_free_pages << (PAGE_SHIFT-10),
	       high_memory >> 10,
	       codepages << (PAGE_SHIFT-10),
	       datapages << (PAGE_SHIFT-10));
	if (emptypages)  printk (", %dk empty", emptypages << (PAGE_SHIFT-10));
	printk (")\n");

	return;
}


/*   Static ramdisk + on-VME-mem-notify stuff.  */

#define MAX_STRD        8
static struct strd_info {
	unsigned int addr;
	unsigned int size;
} strd_info[MAX_STRD] = { {0, 0}, };

static int num_strd = 0;

static int strd_blocksizes[MAX_STRD];

static int strd_open (struct inode * inode, struct file * filp);
static void strd_release (struct inode * inode, struct file * filp);
static int strd_ioctl (struct inode *inode, struct file *file,
				unsigned int cmd, unsigned long arg);
static int strd_mmap (struct inode * inode, struct file * file,
					    struct vm_area_struct * vma);
static void strd_request (void);

static struct file_operations strd_fops = {
	NULL,           /* lseek - default */
	block_read,     /* read - block dev read */
	block_write,    /* write - block dev write */
	NULL,           /* readdir - not here! */
	NULL,           /* select */
	strd_ioctl,     /* ioctl */
	strd_mmap,      /* mmap */
	strd_open,      /* open */
	strd_release,   /* release */
	block_fsync             /* fsync */
};


static void VME_mem_init (struct VME_board *VME, int on_off) {
	struct besta_memory_info *ptr;
	int i;

	if (on_off)  return;    /*  nothing   */

	/*  Looking for more memory (higher than `high_memory'),
	   and make a static ramdisk with them.
	*/
	for (ptr = besta_memory_info; ptr; ptr = ptr->next) {
	    int end_addr = ptr->addr + ptr->size;
	    int addr = ptr->addr < high_memory ? high_memory : ptr->addr;

	    /*  make a static ramdisk, if addr is higher than high_memory   */
	    if (end_addr > addr) {

		strd_info[num_strd].addr = addr;
		strd_info[num_strd].size = end_addr - addr;

		num_strd++;

		ptr->flags |= IN_STRD;
	    }
	}

	if (num_strd) {
	    if (register_blkdev (VME->major, VME->name, &strd_fops)) {
		printk ("  for %s: could not register major %d\n",
						    VME->name, VME->major);
		num_strd = 0;

	    } else {

		blk_dev[VME->major].request_fn = &strd_request;

		for (i = 0; i < num_strd; i++)  strd_blocksizes[i] = 1024;
		blksize_size[VME->major] = strd_blocksizes;
	    }
	}

	/*  If there is an on-VME memory chunk, notify it.  */
	for (ptr = besta_memory_info; ptr; ptr = ptr->next) {
	    if (ptr->time >= MIN_VME_TIME) {
		/*  hear should be per memory-board-type info...  */

		printk ("  0x%08x - 0x%08x: ", ptr->addr,
					       ptr->addr + ptr->size);
		if (ptr->size % (1024 * 1024))
			printk ("%dk ", ptr->size >> 10);
		else
		    printk ("%dMb ", ptr->size >> 20);

		printk ("%d bit memory%s, ", ptr->port * 8,
			    (ptr->flags & CAN_BURST) ? " (burstable)" : "");

		if (!(ptr->flags & IN_STRD))
			printk ("in ordinary usage\n");
		else if (ptr->addr < high_memory)
			printk ("in ordinary and ramdisk usage\n");
		else
		    printk ("used as static ramdisk\n");
	    }
	}

	return;     /*  that is all   */
}


static int strd_open (struct inode * inode, struct file * filp) {

	if (MINOR (inode->i_rdev) >= num_strd)  return -ENXIO;

	return 0;
}

static void strd_release (struct inode * inode, struct file * filp) {

	sync_dev (inode->i_rdev);

	return;
}

static int strd_ioctl (struct inode *inode, struct file *file,
				unsigned int cmd, unsigned long arg) {
	int err;

	switch (cmd) {
	    case BLKFLSBUF:
		if (!suser())  return -EACCES;

		fsync_dev (inode->i_rdev);
		invalidate_buffers (inode->i_rdev);

		break;

	    case BLKGETSIZE:   /* Return device size */
		if (!arg)  return -EINVAL;

		err = verify_area (VERIFY_WRITE, (long *) arg, sizeof(long));
		if (err)  return err;

		put_user (strd_info[MINOR (inode->i_rdev)].size >> 9,
							    (long *) arg);
		break;

	    /*  may be HDIO_GETGEO should be implemented hear ???  */

#ifndef SIOCGETP        /*  svr3  compatible...  */
#define SIOCGETP        (('s'<<8)|6)    /* ioctl: get volume-infos      */
#endif

	    case SIOCGETP:      /*  get parameters (svr3)  */
		{ struct dkinfo {             /* for SIOCGETP         */
		      struct dkvol {
			  ushort  unit;       /* select info                 */
			  ushort  disktype;   /* disk specific type          */
			  ushort  sec_p_track;/* sectors / track             */
			  ushort  hd_offset;  /* add this to head no 0       */
			  ushort  hd_p_vol;   /* heads per volume            */
			  ushort  cyl_p_vol;  /* cylinders per volume        */
			  char    steprate;   /* steprate seek               */
				      /* following infos for format only :   */
			  char    interleave; /* hardwared sector interleave */
			  char    bias_trtr;  /* serpentine track to track   */
			  char    bias_hdhd;  /* serpentine next cylinder    */
		      } vol;
		      struct dkldev {
			  uint    bl_offset;  /* disk block offset           */
			  uint    bl_size;    /* total no of blocks on minor */
		      } ldev;
		  } dkinfo;

		  err = verify_area (VERIFY_WRITE, (void *) arg,
							sizeof (dkinfo));
		  if (err)  return err;

		  dkinfo.vol.unit = MINOR (inode->i_rdev);
		  dkinfo.vol.disktype = 0;
		  dkinfo.vol.sec_p_track = 0;
		  dkinfo.vol.hd_offset = 0;
		  dkinfo.vol.hd_p_vol = 0;
		  dkinfo.vol.cyl_p_vol = 0;
		  dkinfo.vol.steprate = 0;
		  dkinfo.vol.interleave = 0;
		  dkinfo.vol.bias_trtr = 0;
		  dkinfo.vol.bias_hdhd = 0;
		  dkinfo.ldev.bl_offset = 0;
		  dkinfo.ldev.bl_size =
			    strd_info[MINOR (inode->i_rdev)].size >> 10;

		  memcpy_tofs ((void *) arg, &dkinfo, sizeof (dkinfo));
		}

		break;

	    default:
		break;      /*  emulate success, Linux way for rd...  */
	};

	return 0;
}

static int strd_mmap (struct inode * inode, struct file * file,
					    struct vm_area_struct * vma) {
	int minor = MINOR (inode->i_rdev);
	int size = vma->vm_end - vma->vm_start;
	struct besta_memory_info *ptr;
	register unsigned long cacr;

	if (vma->vm_offset & ~PAGE_MASK)  return -EINVAL;
	if (vma->vm_offset + size > strd_info[minor].size)  return -ENXIO;

	__asm volatile ("mov.l %%cacr,%0" : "=d" (cacr) : );

	/*  cacheable, or not ?  */
	for (ptr = besta_memory_info; ptr; ptr = ptr->next)
		if (ptr->addr + ptr->size > strd_info[minor].addr)  break;

	if (!ptr ||
	    (!(ptr->flags & CAN_BURST) && (cacr & 0x1010))
	)  pgprot_val (vma->vm_page_prot) |= _PAGE_NOCACHE030;


	if (remap_page_range (vma->vm_start, vma->vm_offset,
				    size, vma->vm_page_prot))  return -EAGAIN;

	vma->vm_inode = inode;
	inode->i_count++;

	return 0;
}


#undef SECTOR_MASK
#define SECTOR_MASK     ((BLOCK_SIZE >> 9) - 1)

static void end_request (int uptodate) {
	struct request *curr = blk_dev[VME_MAJOR].current_request;
	struct buffer_head *bh;

	curr->errors = 0;

	if (!uptodate) {
	    printk("end_request: I/O error, dev %04lX, sector %lu\n",
			(unsigned long) curr->rq_dev, curr->sector >> 1);

	    curr->nr_sectors--;
	    curr->nr_sectors &= ~SECTOR_MASK;
	    curr->sector += (BLOCK_SIZE/512);
	    curr->sector &= ~SECTOR_MASK;
	}

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

	blk_dev[VME_MAJOR].current_request = curr->next;
	if (curr->sem != NULL)
		up (curr->sem);
	curr->rq_status = RQ_INACTIVE;

	wake_up (&wait_for_request);

	return;
}

static void strd_request (void) {
	unsigned int minor;
	int addr, len;
	struct buffer_head *bh;
	struct request *curr;

	do {
	    curr = blk_dev[VME_MAJOR].current_request;

	    if (!curr || curr->rq_status == RQ_INACTIVE)  return;

	    if (MAJOR (curr->rq_dev) != VME_MAJOR) {
		panic ("strd: (major=%d) request list destroyed !\n",
							    VME_MAJOR);
	    }

	    bh = curr->bh;
	    if(bh && !buffer_locked(bh)) {
		panic ("strd: (major=%d) block not locked !\n", VME_MAJOR);
	    }

	    minor = MINOR (curr->rq_dev);

	    if (minor > num_strd) {
		    end_request(0);
		    continue;
	    }

	    addr = curr->sector << 9;
	    len = curr->current_nr_sectors << 9;

	    if ((addr + len) > strd_info[minor].size) {
		    end_request(0);
		    continue;
	    }

	    addr += strd_info[minor].addr;

	    if (curr->cmd == READ)
		    memcpy (curr->buffer, (void *) addr, len);
	    else
		    memcpy ((void *) addr, curr->buffer, len);

	    end_request(1);

	} while (1);

	return;     /*  not reached   */
}


