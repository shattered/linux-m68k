/*
 * kernel/head-besta.S -- initial boot code for Bestas configuration.
 *
 * Adapted from native `kernel/head.S'.
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

/*
 * Linux startup code.
 *
 * At this point, the boot loader has:
 * Disabled interrupts
 * Disabled caches
 * Put us in supervisor state.
 *
 * Register d3 contains a pointer to boot loader cmdline.
 *
 */

#include <linux/config.h>
#include <linux/linkage.h>
#include <asm/setup.h>
#include <asm/pgtable.h>

.globl SYMBOL_NAME(kernel_pg_dir), SYMBOL_NAME(kpt)
.globl SYMBOL_NAME(availmem), SYMBOL_NAME(is_medusa)
.globl SYMBOL_NAME(m68k_pgtable_cachemode)
.globl SYMBOL_NAME(kernel_pmd_table), SYMBOL_NAME(swapper_pg_dir)

D6B_0460 = 16           /* indicates 680[46]0 in d6 */
D6B_060  = 17           /* indicates 68060 in d6 */
D6F_040  = 1<<D6B_0460
D6F_060  = (1<<D6B_0460)+(1<<D6B_060)

/* Translation control register */
TC_ENABLE = 0x8000
TC_PAGE8K = 0x4000
TC_PAGE4K = 0x0000

/* Transparent translation registers */
TTR_ENABLE      = 0x8000        /* enable transparent translation */
TTR_ANYMODE     = 0x4000        /* user and kernel mode access */
TTR_KERNELMODE  = 0x2000        /* only kernel mode access */
TTR_USERMODE    = 0x0000        /* only user mode access */
TTR_CI          = 0x0400        /* inhibit cache */
TTR_RW          = 0x0200        /* read/write mode */
TTR_RWM         = 0x0100        /* read/write mask */
TTR_FCB2        = 0x0040        /* function code base bit 2 */
TTR_FCB1        = 0x0020        /* function code base bit 1 */
TTR_FCB0        = 0x0010        /* function code base bit 0 */
TTR_FCM2        = 0x0004        /* function code mask bit 2 */
TTR_FCM1        = 0x0002        /* function code mask bit 1 */
TTR_FCM0        = 0x0001        /* function code mask bit 0 */

/* Cache Control registers */
CC6_ENABLE_D    = 0x80000000    /* enable data cache (680[46]0) */
CC6_FREEZE_D    = 0x40000000    /* freeze data cache (68060) */
CC6_ENABLE_SB   = 0x20000000    /* enable store buffer (68060) */
CC6_PUSH_DPI    = 0x10000000    /* disable CPUSH invalidation (68060) */
CC6_HALF_D      = 0x08000000    /* half-cache mode for data cache (68060) */
CC6_ENABLE_B    = 0x00800000    /* enable branch cache (68060) */
CC6_CLRA_B      = 0x00400000    /* clear all entries in branch cache (68060) */
CC6_CLRU_B      = 0x00200000    /* clear user entries in branch cache (68060) */
CC6_ENABLE_I    = 0x00008000    /* enable instruction cache (680[46]0) */
CC6_FREEZE_I    = 0x00004000    /* freeze instruction cache (68060) */
CC6_HALF_I      = 0x00002000    /* half-cache mode for instruction cache (68060) */
CC3_ALLOC_WRITE = 0x00002000    /* write allocate mode(68030) */
CC3_ENABLE_DB   = 0x00001000    /* enable data burst (68030) */
CC3_CLR_D       = 0x00000800    /* clear data cache (68030) */
CC3_CLRE_D      = 0x00000400    /* clear entry in data cache (68030) */
CC3_FREEZE_D    = 0x00000200    /* freeze data cache (68030) */
CC3_ENABLE_D    = 0x00000100    /* enable data cache (68030) */
CC3_ENABLE_IB   = 0x00000010    /* enable instruction burst (68030) */
CC3_CLR_I       = 0x00000008    /* clear instruction cache (68030) */
CC3_CLRE_I      = 0x00000004    /* clear entry in instruction cache (68030) */
CC3_FREEZE_I    = 0x00000002    /* freeze instruction cache (68030) */
CC3_ENABLE_I    = 0x00000001    /* enable instruction cache (68030) */

/* Miscellaneous definitions */
PAGESIZE        = 4096

ROOT_TABLE_SIZE = 128
PTR_TABLE_SIZE  = 128
PAGE_TABLE_SIZE = 64
ROOT_INDEX_SHIFT = 25
PTR_INDEX_SHIFT  = 18
PAGE_INDEX_SHIFT = 12

TABLENR_4MB     = 16    /* # of page tables needed to page 4 MB */
TABLENR_16MB    = 64    /* same for 16 MB */


text
/*0x00000000*/
        space   0x1000

ENTRY(_stext)

.equ    SYMBOL_NAME(kernel_pmd_table),SYMBOL_NAME(_stext)
.equ    SYMBOL_NAME(kernel_pg_dir),SYMBOL_NAME(kernel_pmd_table)
.equ    SYMBOL_NAME(swapper_pg_dir),SYMBOL_NAME(kernel_pg_dir)+(ROOT_TABLE_SIZE<<2)
.equ    Lavail_pmd_table,SYMBOL_NAME(swapper_pg_dir)+(ROOT_TABLE_SIZE<<2)

        space   0x1000

ENTRY(_start)

/*
 * Setup initial stack pointer
 */
        mov.w   &0x2700,%sr     | use interrupt stack
        lea.l   SYMBOL_NAME(_stext),%sp
        mov.l   %sp,%d0
        mov.l   %d0,%msp        | master stack

        sub.l   %a0,%a0
        mov.l   %a0,%vbr

/*
 * Initialize bootinfo, memory, initrd, etc.
 */

        mov.l   %d3,-(%sp)
        jsr     SYMBOL_NAME(besta_bootinfo_init)
        add.l   &4,%sp

        /* save cache mode for page tables */
        mov.l   &0,%d0
        mov.l   %d0,SYMBOL_NAME(m68k_pgtable_cachemode)

/*A*/

/*
 * Get address at end of kernel code/data/bss and
 * mask off at a page boundary.
 */
        lea     SYMBOL_NAME(end),%a0
        mov.l   %a0,%d0
        add.l   &0x0fff,%d0
        and.l   &0xfffff000,%d0
        mov.l   %d0,%fp

/*B*/

/*
 * allocate the kernel root table and initialize it.
 */
        lea     SYMBOL_NAME(kernel_pg_dir),%a5

        /* clear the root table */
        mov.l   %a5,%a0
        mov.l   &ROOT_TABLE_SIZE-1,%d1
1:      clr.l   (%a0)+
        dbra    %d1,1b

/*
 * Initialize root table descriptor pointing to the kernel pointer
 * table.
 */
        lea     SYMBOL_NAME(Lavail_pmd_table),%a4
        mov.l   &0x2,%d0
        add.l   %a4,%d0
        mov.l   %d0,(%a5)

/*C*/

/*
 * Initialize the pointer tables referred to above.  They either point
 * to page tables in the case of the 68040 or contain early
 * termination page descriptors in the case of the 68851 or 68030.
 *
 * Some register usages:
 *    a0 -> pointer table descriptor address
 *    a1 -> pointer table descriptor
 *    d1 -> counter
 *    d2 -> pointer table descriptor increment (varies according to CPU)
 */

        /* clear the kernel pointer table */
        mov.l   %a4,%a0
        mov.l   &PTR_TABLE_SIZE-1,%d1
1:      clr.l   (%a0)+
        dbra    %d1,1b

        mov.l   %a4,%a0
        mov.l   &15,%d1

        /*
         * base value of pointer table descriptor is either
         * the address of the first page table (68040)
         * or the base address of physical memory (68030).
         */

        /* 680[23]0 */
        lea     0.l,%a1         /* base address */
        add.l   &0x1,%a1        /* descriptor type */

2:      mov.l   %a1,(%a0)+
        add.l   &0x40000,%a1
        dbra    %d1,2b

/*D*/

/*
 * Do any machine specific page table initializations.
 */

/*H???*/

/*
 * for the 68030, just setup a translation to map in the last
 * 64M of physical address space using an early termination
 * page descriptors. (Hear are devices, so, without cashing.)
 */

/*I*/

        lea.l   0xfc000000,%a1
        add.w   &0x41,%a1
        mov.l   %a1,504(%a5)
        lea.l   0xfe000000,%a1
        add.w   &0x41,%a1
        mov.l   %a1,508(%a5)

/*J*/

/*
 * Setup Supervisor Root Pointer register to point to page directory,
 * setup translation register contents and enable translation.
 */

/*K*/

        lea     mmu,%a0
        mov.l   &0x80000002,(%a0)   /* no limit, 4byte descriptors */
        mov.l   %a5,4(%a0)
        pmove   (%a0),%srp
        pmove   (%a0),%crp
        pflusha
        /*
         * enable,super root enable,4096 byte pages,7 bit root index,
         * 7 bit pointer index, 6 bit page table index.
         */
        mov.l   &0x82c07760,(%a0)
        pmove   (%a0),%tc                /* enable the MMU */

/*
 * Fixup the addresses for the kernel pointer table and availmem.
 * Convert them from physical addresses to virtual addresses.
 */

/*M*/

        mov.l   %fp,SYMBOL_NAME(availmem)       /* first available memory address */

/*N*/

/*O*/

/*
 * Enable caches
 */
        jsr     SYMBOL_NAME(besta_cache_init)

/*
 * Setup initial stack pointer
 * Note: for mc68060 compatibility we not use master stack
 * (i.e. interrupts go into task kernel stacks).
 */

        lea     SYMBOL_NAME(init_user_stack)+PAGESIZE,%a0
        mov.l   %a0,%d0
        and.l   &0xfffffffc,%d0
        mov.l   %d0,%sp


/* jump to the kernel start */

        jsr     SYMBOL_NAME(start_kernel)

/* and reboot if we are returned hear  */

        mov.l   SYMBOL_NAME(mach_reset),%a0
        jsr     (%a0)
1:
        bra     1b


.globl showtest
showtest:
        rts

        .data
        .even
Lcustom:
Liobase:
        .long 0
mmu:    .quad 0
SYMBOL_NAME_LABEL(kpt)
        .long 0
SYMBOL_NAME_LABEL(availmem)
        .long 0
SYMBOL_NAME_LABEL(is_medusa)
        .long 0
SYMBOL_NAME_LABEL(m68k_pgtable_cachemode)
        .long 0

text
        align   4
data
        align   4
