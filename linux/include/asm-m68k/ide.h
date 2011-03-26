#ifndef _M68K_IDE_H
#define _M68K_IDE_H

/* Copyright(c) 1996 Kars de Jong */
/* Based on the ide driver from 1.2.13pl8 */

#include <linux/config.h>

#ifdef CONFIG_AMIGA
#include <asm/amigahw.h>
#include <asm/amihdreg.h>
#include <asm/amigaints.h>
#endif /* CONFIG_AMIGA */

#ifdef CONFIG_ATARI
#include <asm/atarihw.h>
#include <asm/atarihdreg.h>
#include <asm/atariints.h>
#include <asm/atari_stdma.h>
#endif /* CONFIG_ATARI */

#include <asm/setup.h>

typedef unsigned char * ide_ioreg_t;

#define IDE_MEM_MAPPED_IO

#ifndef MAX_HWIFS
#ifdef CONFIG_AMIGA
#define MAX_HWIFS	4
#else
#define MAX_HWIFS	1
#endif
#endif

#undef SUPPORT_VLB_SYNC
#define SUPPORT_VLB_SYNC 0
#undef SUPPORT_SLOW_DATA_PORTS
#define SUPPORT_SLOW_DATA_PORTS 0

#undef HD_DATA
#define HD_DATA NULL

/* MSch: changed sti() to STI() wherever possible in ide.c; moved STI() def. 
 * to asm/ide.h 
 */
/* The Atari interrupt structure strictly requires that the IPL isn't lowered
 * uncontrolled in an interrupt handler. In the concrete case, the IDE
 * interrupt is already a slow int, so the irq is already disabled at the time
 * the handler is called, and the IPL has been lowered to the minimum value
 * possible. To avoid going below that, STI() checks for being called inside
 * an interrupt, and in that case it does nothing. Hope that is reasonable and
 * works. (Roman)
 */
#if defined(CONFIG_ATARI) && !defined(CONFIG_AMIGA)
#define	STI()					\
    do {					\
	if (!intr_count) sti();			\
    } while(0)
#elif defined(CONFIG_ATARI)
#define	STI()						\
    do {						\
	if (!MACH_IS_ATARI || !intr_count) sti();	\
    } while(0)
#else /* !defined(CONFIG_ATARI) */
#define	STI()	sti()
#endif

#define insl(data_reg, buffer, wcount) insw(data_reg, buffer, (wcount)<<1)
#define outsl(data_reg, buffer, wcount) outsw(data_reg, buffer, (wcount)<<1)

#define insw(port, buf, nr) \
    if ((nr) % 16) \
	__asm__ __volatile__ \
	       ("movel %0,%/a0; \
		 movel %1,%/a1; \
		 movel %2,%/d6; \
		 subql #1,%/d6; \
	       1:movew %/a0@,%/a1@+; \
		 dbra %/d6,1b" : \
		: "g" (port), "g" (buf), "g" (nr) \
		: "a0", "a1", "d6"); \
    else \
	__asm__ __volatile__ \
	       ("movel %0,%/a0; \
		 movel %1,%/a1; \
		 movel %2,%/d6; \
		 lsrl  #4,%/d6; \
		 subql #1,%/d6; \
	       1:movew %/a0@,%/a1@+; \
		 movew %/a0@,%/a1@+; \
		 movew %/a0@,%/a1@+; \
		 movew %/a0@,%/a1@+; \
		 movew %/a0@,%/a1@+; \
		 movew %/a0@,%/a1@+; \
		 movew %/a0@,%/a1@+; \
		 movew %/a0@,%/a1@+; \
		 movew %/a0@,%/a1@+; \
		 movew %/a0@,%/a1@+; \
		 movew %/a0@,%/a1@+; \
		 movew %/a0@,%/a1@+; \
		 movew %/a0@,%/a1@+; \
		 movew %/a0@,%/a1@+; \
		 movew %/a0@,%/a1@+; \
		 movew %/a0@,%/a1@+; \
		 dbra %/d6,1b" : \
		: "g" (port), "g" (buf), "g" (nr) \
		: "a0", "a1", "d6")

#define outsw(port, buf, nr) \
    if ((nr) % 16) \
	__asm__ __volatile__ \
	       ("movel %0,%/a0; \
		 movel %1,%/a1; \
		 movel %2,%/d6; \
		 subql #1,%/d6; \
	       1:movew %/a1@+,%/a0@; \
		 dbra %/d6,1b" : \
		: "g" (port), "g" (buf), "g" (nr) \
		: "a0", "a1", "d6"); \
    else \
	__asm__ __volatile__ \
	       ("movel %0,%/a0; \
		 movel %1,%/a1; \
		 movel %2,%/d6; \
		 lsrl  #4,%/d6; \
		 subql #1,%/d6; \
	       1:movew %/a1@+,%/a0@; \
		 movew %/a1@+,%/a0@; \
		 movew %/a1@+,%/a0@; \
		 movew %/a1@+,%/a0@; \
		 movew %/a1@+,%/a0@; \
		 movew %/a1@+,%/a0@; \
		 movew %/a1@+,%/a0@; \
		 movew %/a1@+,%/a0@; \
		 movew %/a1@+,%/a0@; \
		 movew %/a1@+,%/a0@; \
		 movew %/a1@+,%/a0@; \
		 movew %/a1@+,%/a0@; \
		 movew %/a1@+,%/a0@; \
		 movew %/a1@+,%/a0@; \
		 movew %/a1@+,%/a0@; \
		 movew %/a1@+,%/a0@; \
		 dbra %/d6,1b" : \
		: "g" (port), "g" (buf), "g" (nr) \
		: "a0", "a1", "d6")

#ifdef CONFIG_ATARI
#define insl_swapw(data_reg, buffer, wcount) \
    insw_swapw(data_reg, buffer, (wcount)<<1)
#define outsl_swapw(data_reg, buffer, wcount) \
    outsw_swapw(data_reg, buffer, (wcount)<<1)

#define insw_swapw(port, buf, nr) \
    if ((nr) % 8) \
	__asm__ __volatile__ \
	       ("movel %0,%/a0; \
		 movel %1,%/a1; \
		 movel %2,%/d6; \
		 subql #1,%/d6; \
	       1:movew %/a0@,%/d0; \
		 rolw  #8,%/d0; \
		 movew %/d0,%/a1@+; \
		 dbra %/d6,1b" : \
		: "g" (port), "g" (buf), "g" (nr) \
		: "d0", "a0", "a1", "d6"); \
    else \
	__asm__ __volatile__ \
	       ("movel %0,%/a0; \
		 movel %1,%/a1; \
		 movel %2,%/d6; \
		 lsrl  #3,%/d6; \
		 subql #1,%/d6; \
	       1:movew %/a0@,%/d0; \
		 rolw  #8,%/d0; \
		 movew %/d0,%/a1@+; \
		 movew %/a0@,%/d0; \
		 rolw  #8,%/d0; \
		 movew %/d0,%/a1@+; \
		 movew %/a0@,%/d0; \
		 rolw  #8,%/d0; \
		 movew %/d0,%/a1@+; \
		 movew %/a0@,%/d0; \
		 rolw  #8,%/d0; \
		 movew %/d0,%/a1@+; \
		 movew %/a0@,%/d0; \
		 rolw  #8,%/d0; \
		 movew %/d0,%/a1@+; \
		 movew %/a0@,%/d0; \
		 rolw  #8,%/d0; \
		 movew %/d0,%/a1@+; \
		 movew %/a0@,%/d0; \
		 rolw  #8,%/d0; \
		 movew %/d0,%/a1@+; \
		 movew %/a0@,%/d0; \
		 rolw  #8,%/d0; \
		 movew %/d0,%/a1@+; \
		 dbra %/d6,1b" : \
		: "g" (port), "g" (buf), "g" (nr) \
		: "d0", "a0", "a1", "d6")

#define outsw_swapw(port, buf, nr) \
    if ((nr) % 8) \
	__asm__ __volatile__ \
	       ("movel %0,%/a0; \
		 movel %1,%/a1; \
		 movel %2,%/d6; \
		 subql #1,%/d6; \
	       1:movew %/a1@+,%/d0; \
		 rolw  #8,%/d0; \
		 movew %/d0,%/a0@; \
		 dbra %/d6,1b" : \
		: "g" (port), "g" (buf), "g" (nr) \
		: "d0", "a0", "a1", "d6"); \
    else \
	__asm__ __volatile__ \
	       ("movel %0,%/a0; \
		 movel %1,%/a1; \
		 movel %2,%/d6; \
		 lsrl  #3,%/d6; \
		 subql #1,%/d6; \
	       1:movew %/a1@+,%/d0; \
		 rolw  #8,%/d0; \
		 movew %/d0,%/a0@; \
		 movew %/a1@+,%/d0; \
		 rolw  #8,%/d0; \
		 movew %/d0,%/a0@; \
		 movew %/a1@+,%/d0; \
		 rolw  #8,%/d0; \
		 movew %/d0,%/a0@; \
		 movew %/a1@+,%/d0; \
		 rolw  #8,%/d0; \
		 movew %/d0,%/a0@; \
		 movew %/a1@+,%/d0; \
		 rolw  #8,%/d0; \
		 movew %/d0,%/a0@; \
		 movew %/a1@+,%/d0; \
		 rolw  #8,%/d0; \
		 movew %/d0,%/a0@; \
		 movew %/a1@+,%/d0; \
		 rolw  #8,%/d0; \
		 movew %/d0,%/a0@; \
		 movew %/a1@+,%/d0; \
		 rolw  #8,%/d0; \
		 movew %/d0,%/a0@; \
		 dbra %/d6,1b" : \
		: "g" (port), "g" (buf), "g" (nr) \
		: "d0", "a0", "a1", "d6")
#endif /* CONFIG_ATARI */

#define T_CHAR          (0x0000)        /* char:  don't touch  */
#define T_SHORT         (0x4000)        /* short: 12 -> 21     */
#define T_INT           (0x8000)        /* int:   1234 -> 4321 */
#define T_TEXT          (0xc000)        /* text:  12 -> 21     */

#define T_MASK_TYPE     (0xc000)
#define T_MASK_COUNT    (0x3fff)

#define D_CHAR(cnt)     (T_CHAR  | (cnt))
#define D_SHORT(cnt)    (T_SHORT | (cnt))
#define D_INT(cnt)      (T_INT   | (cnt))
#define D_TEXT(cnt)     (T_TEXT  | (cnt))

static u_short driveid_types[] = {
	D_SHORT(10),	/* config - vendor2 */
	D_TEXT(20),	/* serial_no */
	D_SHORT(3),	/* buf_type - ecc_bytes */
	D_TEXT(48),	/* fw_rev - model */
	D_CHAR(2),	/* max_multsect - vendor3 */
	D_SHORT(1),	/* dword_io */
	D_CHAR(2),	/* vendor4 - capability */
	D_SHORT(1),	/* reserved50 */
	D_CHAR(4),	/* vendor5 - tDMA */
	D_SHORT(4),	/* field_valid - cur_sectors */
	D_INT(1),	/* cur_capacity */
	D_CHAR(2),	/* multsect - multsect_valid */
	D_INT(1),	/* lba_capacity */
	D_SHORT(194)	/* dma_1word - reservedyy */
};

#define num_driveid_types       (sizeof(driveid_types)/sizeof(*driveid_types))

static __inline__ void big_endianize_driveid(struct hd_driveid *id)
{
   u_char *p = (u_char *)id;
   int i, j, cnt;
   u_char t;

   for (i = 0; i < num_driveid_types; i++) {
      cnt = driveid_types[i] & T_MASK_COUNT;
      switch (driveid_types[i] & T_MASK_TYPE) {
         case T_CHAR:
            p += cnt;
            break;
         case T_SHORT:
            for (j = 0; j < cnt; j++) {
               t = p[0];
               p[0] = p[1];
               p[1] = t;
               p += 2;
            }
            break;
         case T_INT:
            for (j = 0; j < cnt; j++) {
               t = p[0];
               p[0] = p[3];
               p[3] = t;
               t = p[1];
               p[1] = p[2];
               p[2] = t;
               p += 4;
            }
            break;
         case T_TEXT:
            for (j = 0; j < cnt; j += 2) {
               t = p[0];
               p[0] = p[1];
               p[1] = t;
               p += 2;
            }
            break;
      }
   }
}

#endif /* _M68K_IDE_H */
